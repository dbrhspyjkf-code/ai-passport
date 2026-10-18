// mail_app —— 未读邮件查看器实现。
// 数据来自 MailStatus(hermes /mail/status)。

#include "mail_app.h"

#include <cstdio>

#include "esp_lvgl_port.h"

LV_FONT_DECLARE(font_home_cn_16);

static const char* TAG = "mailapp";

MailApp& MailApp::Get() {
    static MailApp inst;
    return inst;
}

void MailApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 8, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 序号(右上角):1/3
    index_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(index_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(index_label_, lv_color_hex(0xFFC24B), 0);
    lv_label_set_text(index_label_, "");
    lv_obj_align(index_label_, LV_ALIGN_TOP_RIGHT, 0, 4);

    // 发件人(顶部,绿色)
    from_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(from_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(from_label_, lv_color_hex(0x9FE870), 0);
    lv_obj_set_width(from_label_, LV_HOR_RES - 70);
    lv_label_set_long_mode(from_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(from_label_, "");
    lv_obj_align(from_label_, LV_ALIGN_TOP_LEFT, 0, 4);

    // 主题(白色,最多两行,超出裁剪不显示)
    subject_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(subject_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(subject_label_, lv_color_hex(0xE8E3D3), 0);
    lv_obj_set_width(subject_label_, LV_HOR_RES - 16);
    lv_obj_set_height(subject_label_, 38);   // 两行(行高 ~19)
    lv_label_set_long_mode(subject_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(subject_label_, "");
    lv_obj_align(subject_label_, LV_ALIGN_TOP_LEFT, 0, 30);

    // 分隔线
    lv_obj_t* div = lv_obj_create(root_);
    lv_obj_set_size(div, LV_HOR_RES - 16, 1);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x2A333D), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_set_scrollbar_mode(div, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 74);

    // 内容预览(可滚动区域)
    body_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(body_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(body_label_, lv_color_hex(0xAAB4BE), 0);
    lv_obj_set_width(body_label_, LV_HOR_RES - 16);
    lv_label_set_long_mode(body_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body_label_, "");
    lv_obj_align(body_label_, LV_ALIGN_TOP_LEFT, 0, 84);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void MailApp::Reload() {
    auto& ms = MailStatus::Get();
    msg_count_ = ms.MessageCount();
    if (msg_count_ > 10) msg_count_ = 10;
    for (int i = 0; i < msg_count_; i++) {
        ms.GetMessage(i, msgs_[i].from, sizeof(msgs_[i].from),
                      msgs_[i].subject, sizeof(msgs_[i].subject),
                      msgs_[i].preview, sizeof(msgs_[i].preview));
    }
    if (cur_ >= msg_count_) cur_ = msg_count_ - 1;
    if (cur_ < 0) cur_ = 0;
}

void MailApp::Render() {
    if (root_ == nullptr) return;
    if (msg_count_ <= 0) {
        lv_label_set_text(index_label_, "");
        lv_label_set_text(from_label_, "无未读邮件");
        lv_label_set_text(subject_label_, "");
        lv_label_set_text(body_label_, "上下:退出后自动刷新 · OK:手动刷新");
        return;
    }
    lv_label_set_text_fmt(index_label_, "%d/%d", cur_ + 1, msg_count_);
    lv_label_set_text(from_label_, msgs_[cur_].from);
    lv_label_set_text(subject_label_, msgs_[cur_].subject);
    lv_label_set_text(body_label_, msgs_[cur_].preview);
}

void MailApp::HandleKey(int key) {
    if (!active_) return;
    switch (key) {
        case 0:   // 上:上一封(环绕)
            if (msg_count_ > 0) {
                cur_ = (cur_ + msg_count_ - 1) % msg_count_;
                Render();
            }
            break;
        case 1:   // 下:下一封(环绕)
            if (msg_count_ > 0) {
                cur_ = (cur_ + 1) % msg_count_;
                Render();
            }
            break;
        case 2:   // OK:刷新
            Reload();
            Render();
            break;
        default:
            break;
    }
}

void MailApp::Enter() {
    if (root_ == nullptr) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
        active_ = true;
        Reload();
        Render();
        lvgl_port_unlock();
    }
}

void MailApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        active_ = false;
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

void MailApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        lvgl_port_unlock();
    }
}
