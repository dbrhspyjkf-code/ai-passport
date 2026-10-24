// radio_app.cc —— 网络收音机 v3(复古调谐面板 UI,参考 leo-radio)。
// 设备管线:HTTP 读 PCM → 预缓冲环 → I2S,零解码;服务器端 ffmpeg。
// UI:88-108MHz 刻度盘+指针扫动(lv_anim)+换台白噪声+PCM 峰值电平表。
#include "radio_app.h"

LV_FONT_DECLARE(font_home_cn_16);
LV_FONT_DECLARE(font_pixel_clock_20);

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
};
static const int kStationCount = sizeof(kStations) / sizeof(kStations[0]);

RadioApp& RadioApp::Get() {
    static RadioApp instance;
    return instance;
}

// ---------------- PCM 峰值统计(电平表数据,播放任务写/UI 读) ----------------
static volatile int16_t s_peaks[32];   // 最近 32 个 512B 块的峰值
static volatile int s_peak_wr = 0;
static int16_t s_peak_cur = 0;
static int s_peak_cnt = 0;

static void PeakFeed(const int16_t* data, int samples) {
    for (int i = 0; i < samples; i++) {
        int16_t v = data[i] < 0 ? -data[i] : data[i];
        if (v > s_peak_cur) s_peak_cur = v;
        if (++s_peak_cnt >= 256) {   // 256 样本 = 512 字节一块
            s_peaks[s_peak_wr] = s_peak_cur;
            s_peak_wr = (s_peak_wr + 1) % 32;
            s_peak_cur = 0;
            s_peak_cnt = 0;
        }
    }
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

static int16_t s_drain_buf[1024];

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
        int pos = s_ring_tail;
        uint8_t* dst = reinterpret_cast<uint8_t*>(s_drain_buf);
        for (int i = 0; i < take; i++) {
            dst[i] = s_ring[(pos + i) % s_ring_size];
        }
        s_ring_tail = (pos + take) % s_ring_size;
        codec->OutputData(s_drain_buf, take / 2);   // DMA 满则阻塞=节流
    }
}

static void RingPush(AudioCodec* codec, const int16_t* data, int samples) {
    PeakFeed(data, samples);
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

        // 白噪声调谐声已按用户要求移除(2026-10-24):省 CPU/内存,
        // 且曾与字形渲染争抢缓存。换台静默过渡。

        // 连接(标准顺序 open→fetch_headers→read;LAN 连接快,
        // 服务器 ffmpeg 起流 ~1-2s)
        static uint8_t s_probe[2048];
        int probe_len = 0;
        {
            esp_http_client_config_t hcfg = {};
            hcfg.url = url;
            hcfg.buffer_size = 2048;
            hcfg.buffer_size_tx = 512;
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
            } else {
                esp_http_client_fetch_headers(http);
                int sc = esp_http_client_get_status_code(http);
                int first = esp_http_client_read(http,
                    reinterpret_cast<char*>(s_probe), sizeof(s_probe));
                if (first > 0) {
                    ok = true;
                    probe_len = first;
                    ESP_LOGI(TAG, "relay ok sc=%d first=%dB", sc, first);
                } else {
                    ESP_LOGE(TAG, "relay no data sc=%d", sc);
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
        app->OnStreamState(RadioApp::ST_PLAYING);

        // 环在连接成功后才分配(曾先抢 4K 把最大块吃掉,HTTP 客户端
        // init 随之 NULL——顺序即生死,2026-10-24 实锤)
        {
            int want = 8192;
            uint8_t* ring = static_cast<uint8_t*>(malloc(want));
            if (ring == nullptr) {
                want = 4096;
                ring = static_cast<uint8_t*>(malloc(want));
            }
            s_ring = ring;
            s_ring_size = ring != nullptr ? want : 0;
            s_ring_head = s_ring_tail = 0;
            s_ring_armed = false;
            s_zero_total = 0;
            s_underruns = 0;
            ESP_LOGI(TAG, "ring: %d bytes (after connect)", s_ring_size);
        }

        static uint8_t s_inbuf[2048];
        uint8_t odd_carry = 0;
        bool have_odd = false;
        int zero_reads = 0;

        // 首块探测数据并入播放
        if (probe_len > 0) {
            RingPush(codec, reinterpret_cast<const int16_t*>(s_probe), probe_len / 2);
        }

        while (!app->StopRequested()) {
            int n = esp_http_client_read(http,
                reinterpret_cast<char*>(s_inbuf), sizeof(s_inbuf));
            if (n < 0) { failed = true; break; }
            if (n == 0) {
                s_zero_total++;
                RingDrain(codec);   // 断流期由环内余量续播
                if (++zero_reads > 400) { failed = true; break; }
                vTaskDelay(pdMS_TO_TICKS(15));
                continue;
            }
            zero_reads = 0;
            // 刷新电源定时器活跃戳(防 15s 无活动关输出;每包一次,
            // 128KB/s 下间隔 <100ms 远小于 15s 窗口)
            Application::GetInstance().GetAudioService().NotifyExternalOutput();
            // PCM 16bit 对齐:奇数字节跨读保留拼对
            int off = 0;
            const uint8_t* p = s_inbuf;
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
            RingDrain(codec);
        }
        ESP_LOGI(TAG, "session end: zero_reads=%d underruns=%d",
                 s_zero_total, s_underruns);
        }   // else(连接成功)结束
    }

    if (http != nullptr) esp_http_client_cleanup(http);
    free(s_ring);
    s_ring = nullptr;
    s_ring_size = 0;
    delete s;
    bool was_failed = failed;
    Application::GetInstance().Schedule([app, was_failed]() {
        app->OnStreamState(was_failed ? RadioApp::ST_FAILED : RadioApp::ST_IDLE);
    });
    app->MarkTaskExited();
    // 用户已离开且流已停:恢复协议/音频/轮询
    if (!app->Active()) {
        app->RestoreProtocolIfIdle();
    }
    vTaskDelete(nullptr);
}

// ---------------- UI:复古调谐面板(参考 leo-radio 设计) ----------------

static lv_obj_t* mk_box(lv_obj_t* parent, int x, int y, int w, int h,
                        uint32_t color, int radius = 0) {
    lv_obj_t* r = lv_obj_create(parent);
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

static lv_obj_t* mk_label(lv_obj_t* parent, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
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

void RadioApp::StopNoise() {
    // 调谐噪声已移入播放任务(单写者,连接前 600ms 经环播放);
    // 此函数保留为空操作以兼容调用点
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
    // 台名/频率:预创建 label 显隐切换(零字形渲染)
    for (int i = 0; i < kStationCount; i++) {
        if (name_labels_[i] != nullptr) {
            (i == idx) ? lv_obj_clear_flag(name_labels_[i], LV_OBJ_FLAG_HIDDEN)
                       : lv_obj_add_flag(name_labels_[i], LV_OBJ_FLAG_HIDDEN);
            (i == idx) ? lv_obj_clear_flag(freq_labels_[i], LV_OBJ_FLAG_HIDDEN)
                       : lv_obj_add_flag(freq_labels_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_clear_flag(ch_label_, LV_OBJ_FLAG_HIDDEN);
    int target = NeedleX(FreqOf(idx));
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

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = mk_label(root_, C_FG);
    lv_label_set_text(title, "收音机");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // 预创建全部动态文字(台名×8/频率×8/状态×6):运行期只切换
    // 显隐,零字形渲染——改文本触发 flash XIP 字形渲染,与音频+
    // 网络 DMA 缓存争抢曾致 taskLVGL 卡死 WDT(2026-10-24 实锤)
    for (int i = 0; i < kStationCount; i++) {
        name_labels_[i] = mk_label(root_, C_FG);
        lv_label_set_text(name_labels_[i], kStations[i].name);
        lv_obj_align(name_labels_[i], LV_ALIGN_TOP_MID, 0, 66);
        lv_obj_add_flag(name_labels_[i], LV_OBJ_FLAG_HIDDEN);
        freq_labels_[i] = lv_label_create(root_);
        lv_obj_set_style_text_font(freq_labels_[i], &font_pixel_clock_20, 0);
        lv_obj_set_style_text_color(freq_labels_[i], lv_color_hex(C_AMBER), 0);
        lv_obj_align(freq_labels_[i], LV_ALIGN_TOP_MID, 0, 34);
        lv_obj_add_flag(freq_labels_[i], LV_OBJ_FLAG_HIDDEN);
    }
    ch_label_ = mk_label(root_, C_DIM);
    lv_obj_align(ch_label_, LV_ALIGN_TOP_MID, 0, 90);

    static const char* kStatusTexts[6] = {
        "待机", "正在播放", "已暂停", "连接中…", "调谐中…", "连接失败,换台重试"
    };
    for (int i = 0; i < 6; i++) {
        st_labels_[i] = mk_label(root_, C_FG);
        lv_label_set_text(st_labels_[i], kStatusTexts[i]);
        lv_obj_align(st_labels_[i], LV_ALIGN_TOP_MID, 0, 196);
        lv_obj_add_flag(st_labels_[i], LV_OBJ_FLAG_HIDDEN);
    }
    status_label_ = st_labels_[0];   // 兼容旧引用:当前显示的状态 label

    // 刻度盘面板:41 位刻度(0.5MHz/位),4MHz 主刻度+数字,基线
    lv_obj_t* dial = mk_box(root_, 10, 114, 220, 64, 0x081710, 0);
    const int span = kTrackRight - kTrackLeft;
    for (int i = 0; i <= 40; i++) {
        int x = kTrackLeft - 10 + (long)i * span / 40;
        bool major = (i % 8 == 0);
        bool medium = (i % 2 == 0);
        if (!major && !medium) continue;
        int h = major ? 11 : 6;
        uint32_t col = major ? C_AMBER_SOFT : C_DIM;
        mk_box(dial, x, major ? 8 : 13, major ? 2 : 1, h, col);
        if (major) {
            lv_obj_t* num = mk_label(dial, C_DIM);
            lv_label_set_text_fmt(num, "%d", (kBandLow + i * 5) / 10);
            lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(num, 28);
            lv_obj_set_pos(num, x - 14, 24);
        }
    }
    mk_box(dial, kTrackLeft - 10, 20, span + 1, 2, C_HINT);
    // 指针(3x22 琥珀)+光晕(18x8,20%)
    glow_ = mk_box(dial, 0, 30, 18, 8, C_AMBER, 4);
    lv_obj_set_style_opa(glow_, LV_OPA_20, 0);
    needle_ = mk_box(dial, NeedleX(FreqOf(0)) - 10, 26, 3, 22, C_AMBER, 1);
    lv_obj_set_user_data(needle_, glow_);

    // 电平表已移除(2026-10-24 用户决策):18 个对象 + 100ms 刷新
    // 在播放期持续占用 LVGL 渲染(单核 CPU 争抢,断续帮凶)。
    // 播放动态由状态行文字表达。

    status_label_ = mk_label(root_, C_FG);
    st_show(st_labels_, 0, &status_label_);
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 196);
    mk_box(root_, 16, 224, 208, 1, C_HINT);
    lv_obj_t* hint = mk_label(root_, C_DIM);
    lv_label_set_text(hint, "上下换台 · 确定 播放/暂停 · 长按返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void RadioApp::OnStreamState(int state) {
    if (!active_ || root_ == nullptr) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        switch (state) {
            case ST_PLAYING:
                StopNoise();   // 流已到,停调谐白噪声
                st_show(st_labels_, 1, &status_label_);
                lv_obj_set_style_text_color(status_label_, lv_color_hex(C_FG), 0);
                break;
            case ST_FAILED:
                StopNoise();
                st_show(st_labels_, 5, &status_label_);
                break;
            case ST_IDLE:
            default:
                StopNoise();
                st_show(st_labels_, 2, &status_label_);
                break;
        }
        lvgl_port_unlock();
    }
}

void RadioApp::StartPlay() {
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
    if (xTaskCreate(radio_player_task, "radio_stream", 6144, s, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "player task create failed");
        delete s;
        MarkTaskExited();
        OnStreamState(ST_FAILED);
    }
}

void RadioApp::RestoreProtocolIfIdle() {
    // PCM 版无需重建协议/解码器(从未拆除)。空操作占位。
}

void RadioApp::StopPlay() {
    stop_req_.store(true);   // 任务 ≤1 次读超时内自退
}

void RadioApp::HandleKey(int key) {
    if (!active_ || root_ == nullptr) return;
    switch (key) {
        case 0:   // 上台
        case 1: { // 下台:指针扫动+白噪声+自动播放
            sel_ = (sel_ + (key == 0 ? kStationCount - 1 : 1)) % kStationCount;
            ApplyStation(sel_, true);
            st_show(st_labels_, 4, &status_label_);
            StopPlay();
            lv_timer_create([](lv_timer_t* t) {
                lv_timer_delete(t);
                for (int i = 0; i < 20 && RadioApp::Get().TaskRunning(); i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                RadioApp::Get().StartPlay();
            }, 350, nullptr);
            break;
        }
        case 2:   // OK:播放/暂停
            if (TaskRunning()) {
                StopPlay();
                StopNoise();
                st_show(st_labels_, 2, &status_label_);
            } else {
                st_show(st_labels_, 3, &status_label_);
                    lv_timer_create([](lv_timer_t* t) {
                    lv_timer_delete(t);
                    RadioApp::Get().StartPlay();
                }, 300, nullptr);
            }
            break;
        default:
            break;
    }
}

void RadioApp::Enter() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && !active_) {
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
    StopNoise();
    if (keepalive_timer_ != nullptr) {
        lv_timer_del(keepalive_timer_);
        keepalive_timer_ = nullptr;
    }
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && active_) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            active_ = false;
        }
        lvgl_port_unlock();
    }
}

void RadioApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (vu_timer_ != nullptr) {
            lv_timer_del(vu_timer_);
            vu_timer_ = nullptr;
        }
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        lvgl_port_unlock();
    }
    for (int i = 0; i < kPeaks; i++) bars_[i] = nullptr;
    for (int i = 0; i < kMaxStations; i++) {
        name_labels_[i] = nullptr;
        freq_labels_[i] = nullptr;
    }
    for (int i = 0; i < 6; i++) st_labels_[i] = nullptr;
    ch_label_ = nullptr;
    status_label_ = nullptr;
    needle_ = glow_ = nullptr;
}
