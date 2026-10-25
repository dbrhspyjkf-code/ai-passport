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

    // 页头:主机名(左,亮)
    host_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(host_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(host_label_, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(host_label_, "");
    lv_obj_align(host_label_, LV_ALIGN_TOP_LEFT, 0, 4);

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

    // 居中提示(错误/获取中)
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
        lv_label_set_text(body_label_, "");
        lv_label_set_text(state_label_, "暂无数据");
        return;
    }
    lv_label_set_text(state_label_, "");
    // 数值一律 snprintf 后 set_text:LVGL 内置 printf 不支持 %f
    static char head[64];
    snprintf(head, sizeof(head), "%s · %s", d.hostname, d.uptime);
    lv_label_set_text(host_label_, head);

    const char* fan = "被动散热";
    char fanbuf[16];
    if (d.fan_rpm >= 0) {
        snprintf(fanbuf, sizeof(fanbuf), "%d RPM", d.fan_rpm);
        fan = fanbuf;
    }
    char swap[24] = "";
    if (d.swap_percent >= 0) snprintf(swap, sizeof(swap), " · 交换 %d%%", d.swap_percent);
    static char body[280];
    snprintf(body, sizeof(body),
             "负载 %.2f %.2f %.2f\n"
             "CPU %.1f%%\n"
             "内存 %.1f/%.1fG%s\n"
             "温度 %.0f°C %s\n"
             "IP %s",
             d.load1, d.load5, d.load15,
             d.cpu_percent,
             d.mem_used_mb / 1024.0f, d.mem_total_mb / 1024.0f, swap,
             d.cpu_temp_c, fan, d.ip);
    lv_label_set_text(body_label_, body);
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
        if (!OpiService::Get().Valid()) lv_label_set_text(state_label_, "获取中…");
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
