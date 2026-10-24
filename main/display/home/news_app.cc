// news_app —— AI 新闻查看器实现。
// 数据来自 NewsService(hermes /news/ai,源头 OrangePi cron 午报文件)。

#include "news_app.h"

#include <cstdio>

#include "esp_lvgl_port.h"

#include "news_service.h"

LV_FONT_DECLARE(font_home_cn_16);

NewsApp& NewsApp::Get() {
    static NewsApp inst;
    return inst;
}

void NewsApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 8, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 日期头(左上,灰)
    date_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(date_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(date_label_, lv_color_hex(0x6B7683), 0);
    lv_label_set_text(date_label_, "");
    lv_obj_align(date_label_, LV_ALIGN_TOP_LEFT, 0, 4);

    // 序号(右上,黄):3/11
    index_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(index_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(index_label_, lv_color_hex(0xFFC24B), 0);
    lv_label_set_text(index_label_, "");
    lv_obj_align(index_label_, LV_ALIGN_TOP_RIGHT, 0, 4);

    // 分区名(青):必读/重要/其他/总结
    tag_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(tag_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(tag_label_, lv_color_hex(0x8AE0F7), 0);
    lv_label_set_text(tag_label_, "");
    lv_obj_align(tag_label_, LV_ALIGN_TOP_LEFT, 0, 28);

    // 分隔线
    lv_obj_t* div = lv_obj_create(root_);
    lv_obj_set_size(div, LV_HOR_RES - 16, 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x2A333D), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_set_scrollbar_mode(div, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 54);

    // 正文(白,自动换行;单条≤120 字≈6 行,长文裁剪)
    body_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(body_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(body_label_, lv_color_hex(0xE8E3D3), 0);
    lv_obj_set_width(body_label_, LV_HOR_RES - 16);
    lv_obj_set_height(body_label_, 220);
    lv_label_set_long_mode(body_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body_label_, "");
    lv_obj_align(body_label_, LV_ALIGN_TOP_LEFT, 0, 64);

    // 底部按键提示
    hint_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(hint_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(hint_label_, lv_color_hex(0x6B7683), 0);
    lv_label_set_text(hint_label_, "上下翻阅 · OK刷新 · 长按退出");
    lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_MID, 0, -6);

    // 500ms 检查数据版本:OK 刷新是异步的(由 poller 任务拉取),
    // 数据回来后才重绘——否则按键毫无反馈。
    sync_timer_ = lv_timer_create([](lv_timer_t* t) {
        static_cast<NewsApp*>(lv_timer_get_user_data(t))->SyncFromCache();
    }, 500, this);
    lv_timer_pause(sync_timer_);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

int NewsApp::TotalEntries() const {
    auto& ns = NewsService::Get();
    return ns.ItemCount() + (ns.Summary()[0] != '\0' ? 1 : 0);
}

void NewsApp::Render() {
    if (root_ == nullptr) return;
    auto& ns = NewsService::Get();
    int total = TotalEntries();
    if (total <= 0) {
        lv_label_set_text(index_label_, "");
        lv_label_set_text(date_label_, ns.Valid() ? "" : "暂无新闻");
        lv_label_set_text(tag_label_, "");
        lv_label_set_text(body_label_, "OK 手动刷新,或等待轮询\n(每天中午更新)");
        return;
    }
    if (cur_ >= total) cur_ = total - 1;
    if (cur_ < 0) cur_ = 0;
    lv_label_set_text_fmt(index_label_, "%d/%d", cur_ + 1, total);
    lv_label_set_text(date_label_, ns.Date());
    char label[12], text[124];
    if (cur_ == total - 1 && ns.Summary()[0] != '\0' && cur_ >= ns.ItemCount()) {
        lv_label_set_text(tag_label_, "总结");
        lv_label_set_text(body_label_, ns.Summary());
        return;
    }
    if (ns.GetItem(cur_, label, sizeof(label), text, sizeof(text))) {
        lv_label_set_text(tag_label_, label);
        lv_label_set_text(body_label_, text);
    }
}

void NewsApp::SyncFromCache() {
    if (!active_) return;
    const uint32_t version = NewsService::Get().Version();
    if (version == seen_version_) return;
    seen_version_ = version;
    Render();
}

void NewsApp::HandleKey(int key) {
    if (!active_) return;
    switch (key) {
        case 0:   // 上:上一日条(环绕)
        case 1: { // 下:下一条(环绕)
            int total = TotalEntries();
            if (total > 0) {
                cur_ = (cur_ + (key == 0 ? total - 1 : 1)) % total;
                Render();
            }
            break;
        }
        case 2:   // OK:请求真实拉取(异步),数据到达后由 sync_timer_ 重绘
            NewsService::Get().RequestNow();
            lv_label_set_text(body_label_, "刷新中…");
            break;
        default:
            break;
    }
}

void NewsApp::Enter() {
    if (root_ == nullptr) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
        active_ = true;
        seen_version_ = NewsService::Get().Version();
        if (cur_ >= TotalEntries()) cur_ = 0;
        Render();
        if (sync_timer_ != nullptr) lv_timer_resume(sync_timer_);
        lvgl_port_unlock();
    }
}

void NewsApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        active_ = false;
        if (sync_timer_ != nullptr) lv_timer_pause(sync_timer_);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

void NewsApp::Destroy() {
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
