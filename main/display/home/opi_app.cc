// opi_app —— OrangePi 系统状态查看器实现。
// 数据来自 OpiService(hermes /system/status,与 Mac 桌面 widget 同源)。
// UI 严格复刻 news_app(已验证可显示):头部标签 + 单个多行正文标签。

#include "opi_app.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "opi_service.h"

LV_FONT_DECLARE(font_home_cn_16);

OpiApp& OpiApp::Get() {
    static OpiApp inst;
    return inst;
}

static const uint32_t C_TEXT = 0xE8E3D3, C_DIM = 0x6B7683, C_SEC = 0x2A333D;
static const uint32_t C_GREEN = 0x34D399, C_AMBER = 0xFBBF24, C_RED = 0xEF4444;

static uint32_t tier_color(float pct, float danger, float warn) {
    if (pct >= danger) return C_RED;
    if (pct >= warn) return C_AMBER;
    return C_GREEN;
}

// 进度条:删除重建(创建时即最终尺寸,规避动态改宽度不渲染的坑);
// 轨道背景在 Create 时一次成型(全宽暗条,静态尺寸可渲染)
void OpiApp::RebuildBar(int row, float pct) {
    if (root_ == nullptr) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (bars_[row] != nullptr) {
        lv_obj_delete(bars_[row]);
        bars_[row] = nullptr;
    }
    if (pct <= 0) return;
    // 文字行底 +11px 起条(行距),轨道 y 与填充一致
    static const int kBarY[kBarRows] = {90, 147, 204};   // 嵌在正文空行处
    lv_obj_t* bar = lv_obj_create(root_);
    lv_obj_set_size(bar, (int)(pct / 100 * kBarW + 0.5f), 8);
    lv_obj_set_style_bg_color(bar,
        lv_color_hex(tier_color(pct, row == 0 ? 80 : 85, row == 0 ? 60 : 70)), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, kBarY[row]);
    bars_[row] = bar;
}

void OpiApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 8, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 主机名(左,亮) + IP(右,灰)
    host_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(host_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(host_label_, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(host_label_, "");
    lv_obj_align(host_label_, LV_ALIGN_TOP_LEFT, 0, 4);
    ip_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(ip_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(ip_label_, lv_color_hex(C_DIM), 0);
    lv_label_set_text(ip_label_, "");
    lv_obj_align(ip_label_, LV_ALIGN_TOP_RIGHT, 0, 26);

    // 分隔线(news_app 同款)
    lv_obj_t* div = lv_obj_create(root_);
    lv_obj_set_size(div, LV_HOR_RES - 16, 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(C_SEC), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_set_scrollbar_mode(div, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 54);

    // 正文:全部数据装一个多行标签(news_app body 同款换行模式)
    body_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(body_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(body_label_, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(body_label_, LV_HOR_RES - 16);
    lv_label_set_long_mode(body_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body_label_, "");
    lv_obj_align(body_label_, LV_ALIGN_TOP_LEFT, 0, 64);

    // 进度条轨道背景(一次成型全宽暗条;填充在渲染时删除重建叠在上面)
    static const int kTrackY[kBarRows] = {90, 147, 204};
    for (int r = 0; r < kBarRows; r++) {
        lv_obj_t* track = lv_obj_create(root_);
        lv_obj_set_size(track, kBarW, 8);
        lv_obj_set_style_bg_color(track, lv_color_hex(C_SEC), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(track, 0, 0);
        lv_obj_set_style_radius(track, 0, 0);
        lv_obj_set_style_pad_all(track, 0, 0);
        lv_obj_set_scrollbar_mode(track, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(track, LV_ALIGN_TOP_LEFT, 0, kTrackY[r]);
        bar_track_[r] = track;
    }

    // 居中提示(错误/获取中)——最后创建+显示时置顶,防被轨道/进度条遮住
    state_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(state_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(state_label_, lv_color_hex(C_DIM), 0);
    lv_label_set_text(state_label_, "");
    lv_obj_align(state_label_, LV_ALIGN_TOP_MID, 0, 150);

    // 底部按键提示
    lv_obj_t* hint = lv_label_create(root_);
    lv_obj_set_style_text_font(hint, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_label_set_text(hint, "OK 刷新 · 长按退出");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    // 500ms 检查数据版本(拉取是异步的,数据回来后才重绘)
    sync_timer_ = lv_timer_create([](lv_timer_t* t) {
        static_cast<OpiApp*>(lv_timer_get_user_data(t))->SyncFromCache();
    }, 500, this);
    lv_timer_pause(sync_timer_);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void OpiApp::Render() {
    if (root_ == nullptr) return;
    const OpiService::Data& d = OpiService::Get().data();
    if (!OpiService::Get().Valid()) {
        lv_label_set_text(host_label_, "OrangePi");
        lv_label_set_text(ip_label_, "");
        lv_label_set_text(body_label_, "");
        lv_label_set_text(state_label_, "暂无数据");
        for (int r = 0; r < kBarRows; r++) RebuildBar(r, 0);
        return;
    }
    lv_label_set_text(state_label_, "");
    // 数值一律 snprintf 后 set_text:LVGL 内置 printf 不支持 %f
    static char head[64];
    snprintf(head, sizeof(head), "%s · %s", d.hostname, d.uptime);
    lv_label_set_text(host_label_, head);
    lv_label_set_text(ip_label_, d.ip);

    const char* fan = "被动散热";
    char fanbuf[16];
    if (d.fan_rpm >= 0) {
        snprintf(fanbuf, sizeof(fanbuf), "%d RPM", d.fan_rpm);
        fan = fanbuf;
    }
    char swap[20] = "";
    if (d.swap_percent >= 0) snprintf(swap, sizeof(swap), " 交换%d%%", d.swap_percent);
    static char body[280];
    // 每项数据与自己的进度条一组(文字行 + 双空行,条画在首空行处);
    // 行宽必须 <224px,超宽即折行压条(内存行教训:去"·"分隔符省 32px)
    snprintf(body, sizeof(body),
             "负载 %.2f %.2f %.2f\n\n\n"
             "CPU %.1f%%\n\n\n"
             "内存%.0f%% %.1f/%.1fG%s\n\n\n"
             "温度 %.0f°C %s",
             d.load1, d.load5, d.load15,
             d.cpu_percent,
             d.mem_percent, d.mem_used_mb / 1024.0f, d.mem_total_mb / 1024.0f,
             swap,
             d.cpu_temp_c, fan);
    lv_label_set_text(body_label_, body);
    // 进度条:负载(4 核满载=100%)/CPU/内存
    RebuildBar(0, d.load1 / 4 * 100);
    RebuildBar(1, d.cpu_percent);
    RebuildBar(2, d.mem_percent);
    ESP_LOGI("opiapp", "body(%d):\n%s", (int)strlen(body), body);
}

void OpiApp::SyncFromCache() {
    if (!active_) return;
    const uint32_t version = OpiService::Get().Version();
    if (version == seen_version_) return;
    seen_version_ = version;
    Render();
}

void OpiApp::HandleKey(int key) {
    if (!active_) return;
    switch (key) {
        case 0:   // 上/下:无功能(消费,防落到启动器)
        case 1:
            break;
        case 2:   // OK:请求拉取(异步),数据到达后由 sync_timer_ 重绘
            OpiService::Get().RequestNow();
            lv_obj_move_foreground(state_label_);   // 进度条重建会盖住提示,先置顶
            lv_label_set_text(state_label_, "获取中…");
            break;
        default:
            break;
    }
}

void OpiApp::Enter() {
    if (root_ == nullptr) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
        active_ = true;
        seen_version_ = OpiService::Get().Version();
        Render();
        // 打开即拉一份新鲜的(缓存仅用于瞬时展示,避免白屏等待)
        OpiService::Get().RequestNow();
        if (!OpiService::Get().Valid()) {
            lv_obj_move_foreground(state_label_);   // 进度条重建会盖住提示,先置顶
            lv_label_set_text(state_label_, "获取中…");
        }
        if (sync_timer_ != nullptr) lv_timer_resume(sync_timer_);
        lvgl_port_unlock();
    }
}

void OpiApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        active_ = false;
        if (sync_timer_ != nullptr) lv_timer_pause(sync_timer_);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

void OpiApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        for (int r = 0; r < kBarRows; r++) {
            if (bars_[r] != nullptr) {
                lv_obj_delete(bars_[r]);
                bars_[r] = nullptr;
            }
            if (bar_track_[r] != nullptr) {
                lv_obj_delete(bar_track_[r]);
                bar_track_[r] = nullptr;
            }
        }
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        if (sync_timer_ != nullptr) {
            lv_timer_delete(sync_timer_);   // 定时器独立于对象生命周期,必须显式删除
            sync_timer_ = nullptr;
        }
        lvgl_port_unlock();
    }
}
