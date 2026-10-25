// radio_app.cc —— 网络收音机 v3(复古调谐面板 UI,参考 leo-radio)。
// 设备管线:HTTP 读 PCM → 预缓冲环 → I2S,零解码;服务器端 ffmpeg。
// UI:88-108MHz 刻度盘+指针扫动(lv_anim);换台静默过渡(白噪声/电平
// 表已按用户要求移除,2026-10-24)。
#include "radio_app.h"

LV_FONT_DECLARE(font_home_cn_16);

#include "application.h"
#include "board.h"
#include "display.h"
#include "audio_codec.h"
#include "poller.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "radio";

#define C_BG 0x0a1f14
#define C_FG 0xb7f5c4
#define C_DIM 0x4f7a5e
#define C_HINT 0x34573f
#define C_AMBER 0xf5a263      // 指针琥珀
#define C_AMBER_SOFT 0xb98a5e // 主刻度
#define C_RED 0xf56363

// 电台表:URL 指向转码中继;freq=真实 FM 频率(0.1MHz 单位,0=网络台
// 按列表位置均布)。频率用各地真实频点,网络台读数不编造。
struct RadioStation {
    const char* name;
    const char* url;
    int freq;   // 0.1MHz;0=无 FM 频点(网络台)
};
static const RadioStation kStations[] = {
    {"深圳交通广播", "http://192.168.1.200:8769/radio/szjt", 1062},
    {"深圳新闻广播", "http://192.168.1.200:8769/radio/szxw", 898},
    {"深圳飞扬971", "http://192.168.1.200:8769/radio/szfy", 971},
    {"深圳生活广播", "http://192.168.1.200:8769/radio/szsh", 942},
    {"央广中国之声", "http://192.168.1.200:8769/radio/zgzs", 0},
    {"央广经济之声", "http://192.168.1.200:8769/radio/jjzs", 0},
    {"广东音乐之声", "http://192.168.1.200:8769/radio/gdyy", 993},
    {"羊城交通广播", "http://192.168.1.200:8769/radio/gdyz", 1052},
    {"广东新闻FM91.4", "http://192.168.1.200:8769/radio/gdxw", 914},
    {"珠江经济FM97.4", "http://192.168.1.200:8769/radio/zjjs", 974},
    {"BBC World Service", "http://192.168.1.200:8769/radio/bbcws", 0},
    {"NPR News", "http://192.168.1.200:8769/radio/npr", 0},
    {"KEXP 独立音乐", "http://192.168.1.200:8769/radio/kexp", 0},
    {"Radio Paradise", "http://192.168.1.200:8769/radio/rp", 0},
};
static const int kStationCount = sizeof(kStations) / sizeof(kStations[0]);

RadioApp& RadioApp::Get() {
    static RadioApp instance;
    return instance;
}

// ---------------- PCM 预缓冲环(网络抖动与 I2S 输出解耦) ----------------
// 会话 malloc(失败退化直写);会话串行无并发。热路径全部静态缓冲,
// 零堆依赖(播放期任何 new 都可能 bad_alloc→abort 的教训)。
static uint8_t* s_ring = nullptr;
static int s_ring_size = 0;
static int s_ring_head = 0;
static int s_ring_tail = 0;
static bool s_ring_armed = false;
static int s_zero_total = 0;
static int s_underruns = 0;

static inline int RingUsed() {
    int u = s_ring_head - s_ring_tail;
    return u >= 0 ? u : u + s_ring_size;
}

// HTTP 读缓冲(首块探测与循环读共用;前者喂完环后者才开始读)
static uint8_t s_inbuf[2048];

static void RingDrain(AudioCodec* codec) {
    if (s_ring == nullptr || codec == nullptr) return;
    // 起播水位相对环容量(8K 环 256ms / 4K 环 128ms)
    const int kArmBytes = s_ring_size >= 8192 ? 4096 : s_ring_size / 2;
    if (!s_ring_armed) {
        if (RingUsed() < kArmBytes) return;
        s_ring_armed = true;
    }
    if (RingUsed() == 0) {
        s_underruns++;
        return;
    }
    while (RingUsed() >= 512) {
        int take = RingUsed();
        if (take > 2048) take = 2048;
        take &= ~1;   // 恒取偶数字节:奇数 take 会丢 1 字节→16bit PCM
                      // 对齐永久错位→强噪声(与"丢弃旧数据"同一类坑)
        // 零拷贝:环内连续段直接送 I2S(最多跨尾 2 段)。环容量/tail/
        // take 恒偶数 → 每段都是整样本。曾经按字节取模拷到 2KB 静态
        // 缓冲再写,白耗 CPU 与 2KB 常驻内存。
        int contig = s_ring_size - s_ring_tail;
        if (contig > take) contig = take;
        codec->OutputData(reinterpret_cast<const int16_t*>(s_ring + s_ring_tail),
                          contig / 2);   // DMA 满则阻塞=节流
        s_ring_tail = (s_ring_tail + contig) % s_ring_size;
        if (take > contig) {
            int rest = take - contig;
            codec->OutputData(reinterpret_cast<const int16_t*>(s_ring), rest / 2);
            s_ring_tail = rest;
        }
    }
}

static void RingPush(AudioCodec* codec, const int16_t* data, int samples) {
    if (s_ring == nullptr) {
        codec->OutputData(data, samples);
        return;
    }
    const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
    int len = samples * 2;
    while (len > 0) {
        int free_bytes = s_ring_size - 1 - RingUsed();
        if (free_bytes == 0) {
            // 环满:排空腾位(I2S DMA 满时 OutputData 阻塞=天然背压。
            // 曾用"丢弃旧数据防死锁"——奇数字节丢弃会让 16bit PCM
            // 对齐永久错位 → 强噪声(2026-10-24 "莎莎声"实锤))
            RingDrain(codec);
            free_bytes = s_ring_size - 1 - RingUsed();
            if (free_bytes == 0) {
                vTaskDelay(pdMS_TO_TICKS(5));   // 输出端异常兜底防死旋
            }
            continue;
        }
        int take = len < free_bytes ? len : free_bytes;
        int contig = s_ring_size - s_ring_head;
        if (contig > take) contig = take;
        memcpy(s_ring + s_ring_head, src, contig);
        s_ring_head = (s_ring_head + contig) % s_ring_size;
        src += contig;
        len -= contig;
    }
}

// ---------------- 播放任务(PCM 直通,零解码) ----------------
// 喂入任意字节数(可奇数):奇数字节跨次保留拼对,保证 16bit 样本对齐。
// 首块探测数据与循环读统一走这里(旧 probe 直推 int16 曾可丢 1 字节)。
static void FeedPcm(AudioCodec* codec, const uint8_t* p, int n,
                    uint8_t& odd_carry, bool& have_odd) {
    int off = 0;
    if (have_odd && n >= 1) {
        static uint8_t s_pair[2];
        s_pair[0] = odd_carry;
        s_pair[1] = p[0];
        RingPush(codec, reinterpret_cast<const int16_t*>(s_pair), 1);
        have_odd = false;
        off = 1;
        p += 1;
    }
    int bytes = n - off;
    if (bytes & 1) {
        odd_carry = p[bytes - 1];
        have_odd = true;
        bytes -= 1;
    }
    if (bytes > 0) {
        RingPush(codec, reinterpret_cast<const int16_t*>(p), bytes / 2);
    }
}

struct RadioSession {
    RadioApp* app;
    char url[192];
};

static void radio_player_task(void* arg) {
    RadioSession* s = static_cast<RadioSession*>(arg);
    RadioApp* app = s->app;
    const char* url = s->url;
    bool failed = false;
    esp_http_client_handle_t http = nullptr;

    if (!failed) {
        AudioCodec* codec = Board::GetInstance().GetAudioCodec();
        // 直接写 codec 输出期间,刷新 AudioService 电源定时器的活跃
        // 时间戳(防 15s 关输出)。不再 Stop()/Start() 整个音频服务
        // ——那会反复重建麦克风输入任务,多次后输入路径错乱+分配
        // 风暴(2026-10-24 "聆听中收不到"实锤)。
        codec->EnableOutput(true);
        Application::GetInstance().GetAudioService().NotifyExternalOutput();
        ESP_LOGI(TAG, "session start: free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

        // 白噪声调谐声已按用户要求移除(2026-10-24):省 CPU/内存,
        // 且曾与字形渲染争抢缓存。换台静默过渡。

        // 连接(标准顺序 open→fetch_headers→read;LAN 连接快,
        // 服务器 ffmpeg 起流 ~1-2s)。首块直接读进 s_inbuf(曾另设 2KB
        // s_probe 静态缓冲,与 s_inbuf 从不同时存活,纯浪费)
        int probe_len = 0;
        {
            esp_http_client_config_t hcfg = {};
            hcfg.url = url;
            hcfg.buffer_size = 512;       // 内部收包缓冲:流式读按 512 滚动够用,头 <300B
            hcfg.buffer_size_tx = 256;    // 请求行+头 <200B
            hcfg.timeout_ms = 15000;
            hcfg.max_redirection_count = 4;
            http = esp_http_client_init(&hcfg);
            if (http == nullptr) {
                // 在途轮询轮次把堆压扁:等 3 秒让其收尾后重试一次
                ESP_LOGW(TAG, "init NULL (free=%u largest=%u), retry in 3s",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
                vTaskDelay(pdMS_TO_TICKS(3000));
                http = esp_http_client_init(&hcfg);
            }
            bool ok = false;
            if (http == nullptr) {
                ESP_LOGE(TAG, "init NULL after retry (free=%u largest=%u)",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
            } else if (esp_http_client_open(http, 0) != ESP_OK) {
                ESP_LOGE(TAG, "open failed");
            } else if (app->StopRequested()) {
                // 用户在连接期已换台/停止:立即放弃,不等首块(15s)
            } else {
                esp_http_client_fetch_headers(http);
                if (app->StopRequested()) {
                    ESP_LOGI(TAG, "connect aborted by user");
                } else {
                    int sc = esp_http_client_get_status_code(http);
                    int first = esp_http_client_read(http,
                        reinterpret_cast<char*>(s_inbuf), sizeof(s_inbuf));
                    if (first > 0) {
                        ok = true;
                        probe_len = first;
                        ESP_LOGI(TAG, "relay ok sc=%d first=%dB", sc, first);
                    } else {
                        ESP_LOGE(TAG, "relay no data sc=%d", sc);
                    }
                }
            }
            if (!ok) {
                ESP_LOGE(TAG, "relay connect failed (no data)");
                if (http != nullptr) esp_http_client_cleanup(http);
                http = nullptr;
                failed = true;
            }
        }
        if (failed) {
            // 连接失败也要走完资源清理路径
        } else {
        if (!app->StopRequested()) {
            app->OnStreamState(RadioApp::ST_PLAYING);
        }
        // 流已建立:读超时收紧到 1s——IDF6 里 esp_http_client_read 无数据
        // 超时返回 -ESP_ERR_HTTP_EAGAIN(可容忍断流),其余负值=连接已
        // 死。超时收紧后 StopRequested 的感知延迟从 15s 降到 ~1s。
        esp_http_client_set_timeout_ms(http, 1000);

        // 环仅在堆充裕时才分配(分配后仍需 ≥10K 给 WiFi/lwIP 收包):
        // TCP 接收窗口里排队的音频本身就是抖动缓冲,环与之重复;省电关
        // 闭后到达平滑,直写 I2S(DMA 阻塞=背压)已够。曾强求 8K/4K 环把
        // 播放期堆压到 2.3K→丢包→TCP 指数退避→断流。
        {
            int want = 4096;
            int free_now = (int)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
            uint8_t* ring = nullptr;
            if (free_now - want >= 10240) {
                ring = static_cast<uint8_t*>(malloc(want));
            }
            s_ring = ring;
            s_ring_size = ring != nullptr ? want : 0;
            s_ring_head = s_ring_tail = 0;
            s_ring_armed = false;
            s_zero_total = 0;
            s_underruns = 0;
            ESP_LOGI(TAG, "ring: %d bytes (after connect) free=%u largest=%u", s_ring_size,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        }

        uint8_t odd_carry = 0;
        bool have_odd = false;
        int stall_ticks = 0;

        // 首块探测数据并入播放(与循环读同一奇数字节拼对路径)
        if (probe_len > 0) {
            FeedPcm(codec, s_inbuf, probe_len, odd_carry, have_odd);
        }

        while (!app->StopRequested()) {
            int n = esp_http_client_read(http,
                reinterpret_cast<char*>(s_inbuf), sizeof(s_inbuf));
            if (n == 0) {
                // 服务端正常关闭(HTTP/1.0 EOF):数据不会再来,直接判失败
                // (旧逻辑盲试 400 次≈6s 纯静音等待)
                failed = true;
                break;
            }
            if (n < 0) {
                if (n != -ESP_ERR_HTTP_EAGAIN) { failed = true; break; }
                // 1s 无数据:断流期由环内余量续播;连续 15s(≈旧 15s
                // 读超时的容忍度)仍无数据才判失败——中继端 ffmpeg 重启
                // 重填的间隙(数秒)可自动熬过
                s_zero_total++;
                RingDrain(codec);
                if (++stall_ticks >= 15) { failed = true; break; }
                continue;
            }
            stall_ticks = 0;
            // 刷新电源定时器活跃戳(防 15s 无活动关输出;每包一次,
            // 32KB/s 下间隔 <100ms 远小于 15s 窗口)
            Application::GetInstance().GetAudioService().NotifyExternalOutput();
            FeedPcm(codec, s_inbuf, n, odd_carry, have_odd);
            RingDrain(codec);
        }
        ESP_LOGI(TAG, "session end: stalls=%d underruns=%d",
                 s_zero_total, s_underruns);
        }   // else(连接成功)结束
    }

    if (http != nullptr) esp_http_client_cleanup(http);
    free(s_ring);
    s_ring = nullptr;
    s_ring_size = 0;
    delete s;
    // 栈高水位(字节):用于校准任务栈尺寸——6144 在碎片化堆上已拿不到
    // 连续块(largest 常年 5K),降栈前后以此为据
    ESP_LOGI(TAG, "task exit: stack free min=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    // 先置退出标志再派发终态:终态回调里若新会话已启动(换台续播),
    // 旧会话的"已暂停/失败"作废,不得覆盖"调谐中/正在播放"
    app->MarkTaskExited();
    bool was_failed = failed;
    Application::GetInstance().Schedule([app, was_failed]() {
        if (app->TaskRunning()) return;
        app->OnStreamState(was_failed ? RadioApp::ST_FAILED : RadioApp::ST_IDLE);
    });
    // 用户已离开且流已停:恢复协议/音频/轮询
    if (!app->Active()) {
        app->RestoreProtocolIfIdle();
    }
    vTaskDelete(nullptr);
}

// ---------------- UI:复古调谐面板(参考 leo-radio 设计) ----------------
// LVGL 走系统 malloc(CONFIG_LV_USE_CLIB_MALLOC):HTTPS 轮询把堆打碎后
// lv_*_create 会返回 NULL——曾在刻度盘数字 label 处空指针 panic 重启
// (2026-10-24 实锤)。创建期任何分配失败置标志,Create 分阶段检查后回退。
static bool s_ui_oom = false;

static lv_obj_t* mk_box(lv_obj_t* parent, int x, int y, int w, int h,
                        uint32_t color, int radius = 0) {
    lv_obj_t* r = lv_obj_create(parent);
    if (r == nullptr) { s_ui_oom = true; return nullptr; }
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, radius, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(r, x, y);
    return r;
}

// 共享静态样式(一次初始化,跨 Create/Destroy 复用):每个对象的本地样式
// 属性都是独立小块分配,30 个 label×2 属性曾是收音机 UI 的堆碎片源。
static lv_style_t s_st_lbl_fg, s_st_lbl_amber, s_st_lbl_dim;   // 三色文字

static void init_shared_styles() {
    static bool done = false;
    if (done) return;
    done = true;
    auto text = [](lv_style_t* st, uint32_t color) {
        lv_style_init(st);
        lv_style_set_text_font(st, &font_home_cn_16);
        lv_style_set_text_color(st, lv_color_hex(color));
    };
    text(&s_st_lbl_fg, C_FG);
    text(&s_st_lbl_amber, C_AMBER);
    text(&s_st_lbl_dim, C_DIM);
}

static lv_obj_t* mk_label(lv_obj_t* parent, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    if (l == nullptr) { s_ui_oom = true; return nullptr; }
    lv_style_t* st = color == C_FG ? &s_st_lbl_fg
                   : color == C_AMBER ? &s_st_lbl_amber
                   : color == C_DIM ? &s_st_lbl_dim : nullptr;
    if (st != nullptr) {
        lv_obj_remove_style_all(l);   // 去主题样式,只挂共享样式:零本地分配
        lv_obj_add_style(l, st, 0);
    } else {
        lv_obj_set_style_text_font(l, &font_home_cn_16, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    }
    return l;
}

// 刻度轨道像素范围(与 RadioApp::kTrackLeft/Right 同值,draw 回调用)
static const int kDialTrackL = 20, kDialTrackR = 220;

// 刻度/基线直接画进 dial 的绘制事件:零对象零分配。曾用 22 个小盒子
// 对象(~2KB),在碎片化堆上白占内存——指针动画每一帧都要走这里,
// 纯矩形填充代价可忽略(LVGL 自动按脏区裁剪)。
// 直绘辅助:LVGL9 绘制坐标为屏幕绝对坐标,须加上对象自身坐标偏移
static void draw_rect(lv_layer_t* layer, lv_draw_rect_dsc_t* dsc, const lv_area_t& obj,
                      int x, int y, int w, int h, uint32_t color, int radius = 0) {
    dsc->bg_color = lv_color_hex(color);
    dsc->radius = radius;
    lv_area_t a;
    a.x1 = obj.x1 + x;
    a.y1 = obj.y1 + y;
    a.x2 = a.x1 + w - 1;
    a.y2 = a.y1 + h - 1;
    lv_draw_rect(layer, dsc, &a);
}

static void dial_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t oc;
    lv_obj_get_coords(obj, &oc);
    const int span = kDialTrackR - kDialTrackL;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    for (int i = 0; i <= 40; i++) {
        bool major = (i % 8 == 0);
        if (!major && (i % 2) != 0) continue;
        int x = kDialTrackL - 10 + (long)i * span / 40;
        draw_rect(layer, &dsc, oc, x, major ? 8 : 13, major ? 2 : 1, major ? 11 : 6,
                  major ? C_AMBER_SOFT : C_DIM);
    }
    draw_rect(layer, &dsc, oc, kDialTrackL - 10, 20, span + 1, 2, C_HINT);   // 基线
}

// 竖向布局(屏 240×320):顶部文字三行 → 居中大图标 → 音量条 → 状态 →
// 分隔线 → 两行提示。底部元素按屏高定位,避免再把 320 当 240 排版。
static inline int ScreenH() { return LV_VER_RES; }
static const int kTitleY = 14, kNameY = 78;
static inline int VolY() { return ScreenH() - 102; }       // 音量条
static inline int StatusY() { return ScreenH() - 84; }     // 状态行
static inline int DividerY() { return ScreenH() - 56; }    // 分隔线
static inline int Hint1Y() { return ScreenH() - 50; }
static inline int Hint2Y() { return ScreenH() - 28; }
static const int kVolX = 50, kVolW = 140, kVolH = 4;

// root 背景直绘:屏中央收音机图标(娱乐文件夹同款放大 3×)+音量条+
// 底部分隔线。零对象零分配;音量变化仅失效音量条区域重绘。
static void root_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t oc;
    lv_obj_get_coords(obj, &oc);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    // 图标(机身 90×60 居中):天线/机身/喇叭/指示灯/旋钮
    const int bx = (LV_HOR_RES - 90) / 2, by = 138;
    draw_rect(layer, &dsc, oc, bx + 75, by - 28, 5, 30, C_DIM);
    draw_rect(layer, &dsc, oc, bx, by, 90, 60, C_FG, 10);
    draw_rect(layer, &dsc, oc, bx + 12, by + 15, 30, 30, 0x081710, 15);
    draw_rect(layer, &dsc, oc, bx + 63, by + 15, 12, 12, C_AMBER, 6);
    draw_rect(layer, &dsc, oc, bx + 63, by + 34, 12, 12, 0x081710, 6);
    // 音量条:轨道+填充(宽度∝音量)
    int vol = Board::GetInstance().GetAudioCodec()->output_volume();
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    draw_rect(layer, &dsc, oc, kVolX, VolY(), kVolW, kVolH, C_HINT, 2);
    int fill = kVolW * vol / 100;
    if (fill > 0) draw_rect(layer, &dsc, oc, kVolX, VolY(), fill, kVolH, C_AMBER, 2);
    // 分隔线
    draw_rect(layer, &dsc, oc, 16, DividerY(), 208, 1, C_HINT);
}

// 指针动画回调:同步移动指针+光晕
static void needle_anim_cb(void* var, int32_t v) {
    lv_obj_t* needle = static_cast<lv_obj_t*>(var);
    lv_obj_set_x(needle, v);
    lv_obj_t* glow = static_cast<lv_obj_t*>(lv_obj_get_user_data(needle));
    if (glow != nullptr) lv_obj_set_x(glow, v - 8);
}

int RadioApp::NeedleX(int freq_tenths) const {
    return kTrackLeft + (long)(freq_tenths - kBandLow) * (kTrackRight - kTrackLeft) /
           (kBandHigh - kBandLow);
}

int RadioApp::FreqOf(int idx) const {
    if (kStations[idx].freq != 0) return kStations[idx].freq;
    // 网络台:按列表位置均布(两端内缩,防指针贴边)
    int span = kBandHigh - kBandLow;
    return kBandLow + span / 8 + (long)idx * span * 3 / 4 / (kStationCount - 1);
}

// 状态切换(显隐,零字形渲染):0待机 1播放 2暂停 3连接 4调谐 5失败
static void st_show(lv_obj_t** labels, int idx, lv_obj_t** cur) {
    if (*cur != nullptr) lv_obj_add_flag(*cur, LV_OBJ_FLAG_HIDDEN);
    if (idx >= 0 && labels[idx] != nullptr) {
        lv_obj_clear_flag(labels[idx], LV_OBJ_FLAG_HIDDEN);
        *cur = labels[idx];
    }
}

void RadioApp::ApplyStation(int idx, bool animate) {
    if (root_ == nullptr || idx < 0 || idx >= kStationCount) return;
    // 台名(含频点):预创建 label 显隐切换(零字形渲染)
    for (int i = 0; i < kStationCount; i++) {
        if (name_labels_[i] != nullptr) {
            (i == idx) ? lv_obj_clear_flag(name_labels_[i], LV_OBJ_FLAG_HIDDEN)
                       : lv_obj_add_flag(name_labels_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    int target = NeedleX(FreqOf(idx));
    if (needle_ == nullptr) return;   // 刻度盘关闭(kShowDial=false)
    int cur = lv_obj_get_x(needle_);
    lv_anim_del(needle_, nullptr);
    if (animate && target != cur) {
        int distance = target > cur ? target - cur : cur - target;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, needle_);
        lv_anim_set_exec_cb(&a, needle_anim_cb);
        lv_anim_set_values(&a, cur, target);
        uint32_t dur = 160 + (uint32_t)distance * 3;
        if (dur < 200) dur = 200;
        if (dur > 620) dur = 620;
        lv_anim_set_duration(&a, dur);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else {
        needle_anim_cb(needle_, target);
    }
}

void RadioApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;
    s_ui_oom = false;
    init_shared_styles();
    // 任一阶段对象分配失败:删 root_(连带子对象)、清引用、不进 APP。
    // 启动栏仍可用,堆回暖后再按 OK 会重试创建。
    auto fail = [this](const char* stage) {
        ESP_LOGE(TAG, "UI create failed at %s (LVGL OOM: free=%u largest=%u)", stage,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        if (root_ != nullptr) lv_obj_delete(root_);
        ResetUiRefs();
    };

    root_ = lv_obj_create(parent);
    if (root_ == nullptr) { fail("root"); return; }
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);   // 先隐藏,Enter 再显示
    lv_obj_add_event_cb(root_, root_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);   // 图标/音量条/分隔线

    lv_obj_t* title = mk_label(root_, C_FG);
    if (title == nullptr) { fail("title"); return; }
    lv_label_set_text(title, "收音机");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kTitleY);

    // 预创建全部动态文字(台名×8/频率×8/状态×6):运行期只切换
    // 显隐,零字形渲染——改文本触发 flash XIP 字形渲染,与音频+
    // 网络 DMA 缓存争抢曾致 taskLVGL 卡死 WDT(2026-10-24 实锤)。
    // 频率读数用 cn 字体:pixel 字体只有 0-9/: 装不下 ".MHz";
    // 网络台无 FM 频点,读数不编造。频道号 "n / 8" 已放弃(多 8 个对象
    // 曾把碎片化的堆压爆→panic)。
    // 每台一个 label:"频点 · 台名"(网络台"网络 · 台名")。曾拆名称/频率
    // 两行×N 台,14 台时 28 个 label 把播放期堆压到 2.5K(largest 576B)
    // → WDT,合并后对齐内存预算。
    for (int i = 0; i < kStationCount; i++) {
        name_labels_[i] = mk_label(root_, C_FG);
        if (name_labels_[i] == nullptr) break;
        if (kStations[i].freq != 0) {
            lv_label_set_text_fmt(name_labels_[i], "%d.%d · %s",
                                  kStations[i].freq / 10, kStations[i].freq % 10,
                                  kStations[i].name);
        } else {
            lv_label_set_text_fmt(name_labels_[i], "网络 · %s", kStations[i].name);
        }
        lv_obj_align(name_labels_[i], LV_ALIGN_TOP_MID, 0, kNameY);
        lv_obj_add_flag(name_labels_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui_oom) { fail("station labels"); return; }

    static const char* kStatusTexts[6] = {
        "待机", "正在播放", "已暂停", "连接中…", "调谐中…", "连接失败,换台重试"
    };
    for (int i = 0; i < 6; i++) {
        st_labels_[i] = mk_label(root_, C_FG);
        if (st_labels_[i] == nullptr) break;
        lv_label_set_text(st_labels_[i], kStatusTexts[i]);
        lv_obj_align(st_labels_[i], LV_ALIGN_TOP_MID, 0, StatusY());
        lv_obj_add_flag(st_labels_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui_oom) { fail("status labels"); return; }
    lv_obj_set_style_text_color(st_labels_[5], lv_color_hex(C_RED), 0);   // 失败=红
    status_label_ = st_labels_[0];   // 当前显示的状态 label
    st_show(st_labels_, 0, &status_label_);

    // 刻度盘(dial+6 数字+指针+光晕≈1.5KB):2026-10-24 晚按用户决定关闭以
    // 给播放期 WiFi 收包留堆(播放期 free 曾只剩 2.3K→丢包→TCP 退避→断流)。
    // 恢复:kShowDial=true 即可,ApplyStation/anim 路径都保留。
    static const bool kShowDial = false;
    if (kShowDial) {
    lv_obj_t* dial = mk_box(root_, 10, 114, 220, 64, 0x081710, 0);
    if (dial == nullptr) { fail("dial"); return; }
    lv_obj_add_event_cb(dial, dial_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    const int span = kTrackRight - kTrackLeft;
    for (int i = 0; i <= 40; i += 8) {
        int x = kTrackLeft - 10 + (long)i * span / 40;
        lv_obj_t* num = mk_label(dial, C_DIM);
        if (num == nullptr) break;
        lv_label_set_text_fmt(num, "%d", (kBandLow + i * 5) / 10);
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(num, 28);
        lv_obj_set_pos(num, x - 14, 24);
    }
    // 指针(3x22 琥珀)+光晕(18x8,20%)
    glow_ = mk_box(dial, 0, 30, 18, 8, C_AMBER, 4);
    needle_ = mk_box(dial, NeedleX(FreqOf(0)) - 10, 26, 3, 22, C_AMBER, 1);
    if (s_ui_oom || glow_ == nullptr || needle_ == nullptr) { fail("dial/needle"); return; }
    lv_obj_set_style_opa(glow_, LV_OPA_20, 0);
    lv_obj_set_user_data(needle_, glow_);
    }

    // 电平表已移除(2026-10-24 用户决策):18 个对象 + 100ms 刷新
    // 在播放期持续占用 LVGL 渲染(单核 CPU 争抢,断续帮凶)。
    // 播放动态由状态行文字表达。

    // 分隔线由 root_draw_cb 直绘;两行按键提示按屏高从底部定位
    lv_obj_t* hint1 = mk_label(root_, C_DIM);
    lv_obj_t* hint2 = mk_label(root_, C_DIM);
    if (s_ui_oom || hint1 == nullptr || hint2 == nullptr) { fail("hint"); return; }
    lv_label_set_text(hint1, "短按 音量 · 长按 频道列表");
    lv_obj_align(hint1, LV_ALIGN_TOP_MID, 0, Hint1Y());
    lv_label_set_text(hint2, "OK 播放/暂停 · 长按 OK 返回");
    lv_obj_align(hint2, LV_ALIGN_TOP_MID, 0, Hint2Y());

    // ---- 频道列表浮层(长按上/下弹出):滚动面板+预创建行+移动高亮条 ----
    // 行高 22px 固定(行绝对定位,不依赖字体行距);列表 UI 创建失败时
    // 长按上/下降级为直接顺序换台(见 HandleKey)。
    list_panel_ = lv_obj_create(root_);
    if (list_panel_ != nullptr) {
        lv_obj_set_size(list_panel_, 232, 268);
        lv_obj_set_style_bg_color(list_panel_, lv_color_hex(0x06120c), 0);
        lv_obj_set_style_bg_opa(list_panel_, LV_OPA_90, 0);
        lv_obj_set_style_border_color(list_panel_, lv_color_hex(C_HINT), 0);
        lv_obj_set_style_border_width(list_panel_, 1, 0);
        lv_obj_set_style_radius(list_panel_, 0, 0);
        lv_obj_set_style_pad_all(list_panel_, 0, 0);
        lv_obj_set_scrollbar_mode(list_panel_, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_add_flag(list_panel_, LV_OBJ_FLAG_SCROLLABLE);   // 需滚动
        lv_obj_set_pos(list_panel_, 4, 26);
        lv_obj_add_flag(list_panel_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* lt = mk_label(list_panel_, C_DIM);
        if (lt != nullptr) {
            lv_label_set_text(lt, "频道列表 · OK 播放 · 长按收起");
            lv_obj_set_pos(lt, 10, 4);
        }
        // 单个多行 label 承载全部台名(行距补齐到 22px,指示条按行定位),
        // 比 14 个行 label 省 ~1.7K;文本只在 Create 设置一次
        const int lh = lv_font_get_line_height(&font_home_cn_16);
        lv_obj_t* rows = mk_label(list_panel_, C_FG);
        if (rows != nullptr) {
            static char list_text[kMaxStations * 32 + 64];
            int off = 0;
            for (int i = 0; i < kStationCount; i++) {
                off += snprintf(list_text + off, sizeof(list_text) - off,
                                "%d %s\n", i + 1, kStations[i].name);
            }
            if (off > 0 && list_text[off - 1] == '\n') list_text[off - 1] = '\0';
            lv_label_set_text(rows, list_text);
            lv_obj_set_style_text_line_space(rows, kListRow - lh, 0);
            lv_obj_set_pos(rows, 16, kListTop);
        }
        list_bar_ = mk_box(list_panel_, 6, kListTop, 4, lh, C_AMBER, 2);
        if (s_ui_oom || rows == nullptr || list_bar_ == nullptr) {
            // 列表建一半失败:整体放弃,保留"直接顺序换台"降级路径
            lv_obj_delete(list_panel_);
            list_panel_ = nullptr;
            list_bar_ = nullptr;
            s_ui_oom = false;
            ESP_LOGW(TAG, "station list UI unavailable (OOM)");
        }
    }
}

void RadioApp::OnStreamState(int state) {
    if (!active_ || root_ == nullptr) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        // 有新会话待启动(换台/OK 续播):当前任务已是过期会话,
        // 其任何状态都不覆盖"调谐中…/连接中…"
        if (start_timer_ == nullptr) {
            switch (state) {
                case ST_PLAYING:
                    st_show(st_labels_, 1, &status_label_);
                    break;
                case ST_FAILED:
                    st_show(st_labels_, 5, &status_label_);
                    break;
                case ST_IDLE:
                default:
                    st_show(st_labels_, 2, &status_label_);
                    break;
            }
        }
        lvgl_port_unlock();
    }
}

void RadioApp::StartPlay() {
    if (!active_) return;   // 用户已退出:绝不启动幽灵播放
    if (TaskRunning()) return;
    if (sel_ < 0 || sel_ >= kStationCount) return;
    // 早上验证过的极简配置:字形缓存清 + poller 停。不碰 cork/
    // 不做预检重启(下午的连锁崩溃均来自这些"优化")。
    Board::GetInstance().GetDisplay()->ReleaseCachesForMemory();
    Poller::Get().Stop();
    MarkTaskRunning();
    stop_req_.store(false);
    RadioSession* s = new RadioSession();
    s->app = this;
    strncpy(s->url, kStations[sel_].url, sizeof(s->url) - 1);
    s->url[sizeof(s->url) - 1] = 0;
    // 栈 6144→3072:HTTPS 轮询把堆打碎后 largest 常年 ~5K,6K 连续块
    // 开机十几秒后就再也拿不到(→"player task create failed"→换台即
    // "连接失败")。纯 HTTP 无 TLS;实测含流播放的高水位峰值 ~1.4KB
    // ("task exit: stack free min" 日志),3072 留 >100% 余量。
    if (xTaskCreate(radio_player_task, "radio_stream", 3072, s, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "player task create failed (free=%u largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        delete s;
        MarkTaskExited();
        OnStreamState(ST_FAILED);
    }
}

void RadioApp::RestoreProtocolIfIdle() {
    // PCM 版无需重建协议/解码器(从未拆除)。空操作占位。
}

// 延时启动(换台 350ms 防抖/OK 300ms):旧任务未退出则每 100ms 轮询,
// 全程不阻塞 LVGL 任务——旧实现 vTaskDelay 等待最长 2s,期间持 LVGL
// 锁,指针动画冻结/按键被丢/超时后 StartPlay 落空(换台后不播的根因)
void RadioApp::ScheduleStart(uint32_t delay_ms) {
    if (start_timer_ != nullptr) {
        lv_timer_set_period(start_timer_, delay_ms);
        lv_timer_reset(start_timer_);
        return;
    }
    start_timer_ = lv_timer_create([](lv_timer_t* t) {
        RadioApp& app = RadioApp::Get();
        if (app.TaskRunning()) {
            lv_timer_set_period(t, 100);   // 旧任务收尾中:继续轮询
            return;
        }
        lv_timer_delete(t);
        app.start_timer_ = nullptr;
        app.StartPlay();
    }, delay_ms, nullptr);
}

void RadioApp::CancelStartTimer() {
    if (start_timer_ != nullptr) {
        lv_timer_delete(start_timer_);
        start_timer_ = nullptr;
    }
}

void RadioApp::StopPlay() {
    stop_req_.store(true);   // 任务 ≤1 次读超时内自退
}

void RadioApp::PlayStation(int idx) {
    // 选台播放:停旧流 + 350ms 防抖后等旧任务退出再启动(见 ScheduleStart)
    sel_ = idx;
    ApplyStation(sel_, false);
    st_show(st_labels_, 4, &status_label_);
    StopPlay();
    ScheduleStart(350);
}

void RadioApp::ChangeStation(int dir) {
    PlayStation((sel_ + dir + kStationCount) % kStationCount);
}

void RadioApp::OpenStationList() {
    if (list_panel_ == nullptr) return;   // UI 未建成(堆碎片):降级直接换台
    list_sel_ = sel_;
    UpdateListHighlight();
    lv_obj_clear_flag(list_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(list_panel_);
    list_open_ = true;
}

void RadioApp::CloseStationList() {
    if (list_panel_ != nullptr) {
        lv_obj_add_flag(list_panel_, LV_OBJ_FLAG_HIDDEN);
    }
    list_open_ = false;
}

void RadioApp::MoveListSel(int d) {
    list_sel_ = (list_sel_ + d + kStationCount) % kStationCount;
    UpdateListHighlight();
}

void RadioApp::UpdateListHighlight() {
    if (list_bar_ == nullptr) return;
    lv_obj_set_pos(list_bar_, 6, kListTop + list_sel_ * kListRow);
    lv_obj_scroll_to_view(list_bar_, LV_ANIM_OFF);   // 选中行滚进可见区
}

void RadioApp::AdjustVolume(int delta) {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    int v = codec->output_volume() + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    codec->SetOutputVolume(v);   // 硬件音量 + NVS 持久化(与主页静音/最大同路径)
    // 只失效音量条区域(含圆角边缘),不触发整屏/字形重绘
    lv_area_t a;
    lv_obj_get_coords(root_, &a);
    a.x1 += kVolX - 2;  a.y1 += VolY() - 2;
    a.x2 = a.x1 + kVolW + 3;  a.y2 = a.y1 + kVolH + 3;
    lv_obj_invalidate_area(root_, &a);
}

void RadioApp::HandleKey(int key) {
    if (!active_ || root_ == nullptr) return;
    switch (key) {
        case 0:   // UP 短按:列表开=选上台,列表关=音量+
            if (list_open_) MoveListSel(-1);
            else AdjustVolume(+10);
            break;
        case 1:   // DOWN 短按:列表开=选下台,列表关=音量-
            if (list_open_) MoveListSel(+1);
            else AdjustVolume(-10);
            break;
        case 3:   // UP 长按:列表开关切换(无列表 UI 时降级上一台)
        case 4:   // DOWN 长按:同上(两键均为开关)
            if (list_panel_ == nullptr) {
                ChangeStation(key == 3 ? -1 : +1);
            } else if (list_open_) {
                CloseStationList();
            } else {
                OpenStationList();
            }
            break;
        case 2:   // OK:列表开=播放选中并返回播放页;列表关=播放/暂停
            if (list_open_) {
                int pick = list_sel_;
                CloseStationList();
                if (pick != sel_ || !TaskRunning()) {
                    PlayStation(pick);
                }
                break;
            }
            if (TaskRunning() && !StopRequested()) {
                CancelStartTimer();
                StopPlay();
                st_show(st_labels_, 2, &status_label_);
            } else {
                // 未播放,或正在停止中(换台余波)→ (重新)启动
                st_show(st_labels_, 3, &status_label_);
                ScheduleStart(300);
            }
            break;
        default:
            break;
    }
}

void RadioApp::Enter() {
    if (root_ == nullptr) return;   // Create 因堆碎片失败:不动任何系统状态
    // 流媒体会话关 WiFi 省电(退出恢复 MAX_MODEM)。实测:省电开着时
    // OrangePi ping 设备 RTT~230ms/丢包 25%,中继已回 200 而设备 15s
    // 收不到一个字节(HTTP_CLIENT "timed out before data was ready")。
    // MQTT/轮询的小请求侥幸能过,流式响应必死。
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (!active_) {
            lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
            active_ = true;
            ApplyStation(sel_, false);
        }
        lvgl_port_unlock();
    }
    // 会话期保活:每秒刷新电源定时器活跃戳 → 输出永不关闭 →
    // 播放时 EnableOutput 恒为空操作 → I2S 零重配置(duplex RX
    // 停摆/wifi m f null 连锁卡死的根源)+音频任务零重建(麦克风
    // 输入任务复用,Stop/Start 循环曾致"聆听中收不到")。
    // 不 Stop 整个 AudioService,不碰 cork。
    if (keepalive_timer_ == nullptr) {
        keepalive_timer_ = lv_timer_create(
            [](lv_timer_t*) {
                Application::GetInstance().GetAudioService().NotifyExternalOutput();
            }, 1000, nullptr);
    }
    // 释放 TLS 软木塞:它持有的 11K 把最大块切成 ~8K,收音机
    // HTTP/pbuf/环全程吃紧(连接失败/断续主因)。MQTT 掉线自愈
    // 在收音机会话期间牺牲——退出后 clock tick 自动重持。
    Application::GetInstance().ReleaseTlsCork();
    // 进 APP 即停轮询(不等按播放):轮询器的 HTTPS 突发(GLM/
    // DeepSeek TLS 握手 2×4.7K)把堆撕到 largest 1.5K,HTTP
    // 客户端 2.5K 装不下→init NULL 瞬间失败(2026-10-24 实锤)
    Poller::Get().Stop();
}

void RadioApp::Leave() {
    StopPlay();
    CloseStationList();   // 列表随会话收起(状态标志复位)
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);   // 恢复省电(见 Enter 注释)
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        CancelStartTimer();   // 待启动的续播随会话作废(防退出后幽灵播放)
        if (keepalive_timer_ != nullptr) {
            lv_timer_delete(keepalive_timer_);
            keepalive_timer_ = nullptr;
        }
        if (root_ != nullptr && active_) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            active_ = false;
        }
        lvgl_port_unlock();
    }
}

void RadioApp::ResetUiRefs() {
    root_ = nullptr;
    for (int i = 0; i < kMaxStations; i++) {
        name_labels_[i] = nullptr;
    }
    for (int i = 0; i < 6; i++) st_labels_[i] = nullptr;
    list_panel_ = nullptr;
    list_bar_ = nullptr;
    list_open_ = false;
    status_label_ = nullptr;
    needle_ = glow_ = nullptr;
}

void RadioApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
        }
        ResetUiRefs();
        lvgl_port_unlock();
    }
}
