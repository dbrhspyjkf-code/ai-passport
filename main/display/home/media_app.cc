// media_app.cc —— 播放文件夹选择屏(音乐播放器 Sonos/HomePod + 收音机)。
#include "media_app.h"

LV_FONT_DECLARE(font_home_cn_16);
LV_FONT_DECLARE(font_material_symbols_20_4);

#include "material_symbols.h"
#include "home_screen.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

#define C_BG 0x0a1f14
#define C_FG 0xb7f5c4
#define C_DIM 0x4f7a5e

static const char* TAG = "media";

MediaApp& MediaApp::Get() {
    static MediaApp instance;
    return instance;
}

// 小矩形工具(与 games_app 同款,可选圆角)
static lv_obj_t* mk_rect(lv_obj_t* parent, int w, int h, uint32_t color, int radius = 0) {
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, radius, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

void MediaApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    if (root_ == nullptr) return;   // 堆碎片化时创建失败:Enter 判空直接返回
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t* title = lv_label_create(root_);
    if (title == nullptr) { lv_obj_delete(root_); root_ = nullptr; return; }
    lv_obj_set_style_text_font(title, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_FG), 0);
    lv_label_set_text(title, "播放");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    // 两个条目行(图标左,名称+副题右);屏 240×320,两行垂直居中偏上
    static const char* kNames[kEntryCount] = {"音乐播放器", "收音机"};
    static const char* kSubs[kEntryCount] = {"Sonos · HomePod", "网络电台"};
    for (int i = 0; i < kEntryCount; i++) {
        lv_obj_t* row = lv_obj_create(root_);
        if (row == nullptr) { lv_obj_delete(root_); root_ = nullptr; return; }
        lv_obj_set_size(row, 208, 60);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x081710), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 104 + i * 80);
        rows_[i] = row;

        lv_obj_t* name = lv_label_create(row);
        lv_obj_t* sub = lv_label_create(row);
        if (name == nullptr || sub == nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
            for (int k = 0; k < kEntryCount; k++) rows_[k] = nullptr;
            ESP_LOGE(TAG, "UI create failed (OOM)");
            return;
        }
        lv_obj_set_style_text_font(name, &font_home_cn_16, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_FG), 0);
        lv_label_set_text(name, kNames[i]);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 64, 7);
        lv_obj_set_style_text_font(sub, &font_home_cn_16, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(C_DIM), 0);
        lv_label_set_text(sub, kSubs[i]);
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 64, 33);
    }

    // 行图标 0:音符(与主页"播放"格同款 Material Symbols)
    lv_obj_t* note = lv_label_create(rows_[0]);
    lv_obj_set_style_text_font(note, &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(C_FG), 0);
    lv_label_set_text(note, MATERIAL_SYMBOLS_MUSIC_NOTE);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 20, 17);
    // 行图标 1:收音机(机身+天线+喇叭点阵,与娱乐文件夹同款)
    lv_obj_t* rbody = mk_rect(rows_[1], 30, 20, 0xb7f5c4, 3);
    lv_obj_set_pos(rbody, 12, 30);
    lv_obj_t* rant = mk_rect(rows_[1], 2, 12, 0x4f7a5e);
    lv_obj_set_pos(rant, 38, 18);
    lv_obj_t* rspk = mk_rect(rows_[1], 10, 10, 0x081710, 5);
    lv_obj_set_pos(rspk, 17, 35);
    lv_obj_t* rdot = mk_rect(rows_[1], 4, 4, 0xf5a263, 2);
    lv_obj_set_pos(rdot, 33, 33);

    // 底部按键提示
    lv_obj_t* hint = lv_label_create(root_);
    lv_obj_set_style_text_font(hint, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_label_set_text(hint, "OK 进入 · 长按返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void MediaApp::Highlight() {
    for (int i = 0; i < kEntryCount; i++) {
        if (rows_[i] == nullptr) continue;
        lv_obj_set_style_border_color(rows_[i],
            lv_color_hex(i == sel_ ? C_FG : C_DIM), 0);
        lv_obj_set_style_border_width(rows_[i],
            i == sel_ ? 2 : 1, 0);
    }
}

void MediaApp::HandleKey(int key) {
    if (!active_ || root_ == nullptr) return;
    switch (key) {
        case 0:   // 上:上一项(循环)
            sel_ = (sel_ + kEntryCount - 1) % kEntryCount;
            Highlight();
            break;
        case 1:   // 下:下一项(循环)
            sel_ = (sel_ + 1) % kEntryCount;
            Highlight();
            break;
        case 2:   // OK:进入选中 APP(SwitchApp 会销毁本文件夹)
            HomeScreen::Get().SwitchApp(sel_ == 0 ? 3 : 8);   // 3=播放器 8=收音机
            break;
        default:
            break;
    }
}

void MediaApp::Enter() {
    if (root_ == nullptr) return;   // Create 失败(堆碎片):不动任何状态
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (!active_) {
            lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
            active_ = true;
            Highlight();
        }
        lvgl_port_unlock();
    }
}

void MediaApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        lvgl_port_unlock();
    }
    for (int i = 0; i < kEntryCount; i++) rows_[i] = nullptr;
}

void MediaApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && active_) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            active_ = false;
        }
        lvgl_port_unlock();
    }
}
