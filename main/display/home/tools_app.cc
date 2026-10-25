// tools_app.cc —— 工具文件夹选择屏实现(新闻/OrangePi)。
#include "tools_app.h"

LV_FONT_DECLARE(font_home_cn_16);

#include "home_screen.h"

#include <cstdio>

#include "esp_lvgl_port.h"

// 深蓝工具风(区别于娱乐文件夹的墨绿)
#define C_BG 0x0a1622
#define C_FG 0x9fd8e8
#define C_DIM 0x46647a
#define C_HINT 0x2f4657

ToolsApp& ToolsApp::Get() {
    static ToolsApp instance;
    return instance;
}

void ToolsApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t* title = lv_label_create(root_);
    lv_obj_set_style_text_font(title, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_FG), 0);
    lv_label_set_text(title, "工具");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    // 两个条目行(图标左,名称+说明右)
    static const char* kNames[kToolCount] = {"AI 新闻", "ORANGEPI"};
    static const char* kSubs[kToolCount] = {"每天中午更新", "系统状态监控"};
    for (int i = 0; i < kToolCount; i++) {
        lv_obj_t* row = lv_obj_create(root_);
        lv_obj_set_size(row, 208, 60);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x081120), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 84 + i * 88);
        rows_[i] = row;

        lv_obj_t* name = lv_label_create(row);
        lv_obj_set_style_text_font(name, &font_home_cn_16, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_FG), 0);
        lv_label_set_text(name, kNames[i]);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 64, 7);

        lv_obj_t* sub = lv_label_create(row);
        lv_obj_set_style_text_font(sub, &font_home_cn_16, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(C_DIM), 0);
        lv_label_set_text(sub, kSubs[i]);
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 64, 33);
    }

    // 行图标 0:新闻"闻"字(青)
    lv_obj_t* news_icon = lv_label_create(rows_[0]);
    lv_obj_set_style_text_font(news_icon, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(news_icon, lv_color_hex(0x8ae0f7), 0);
    lv_label_set_text(news_icon, "闻");
    lv_obj_align(news_icon, LV_ALIGN_TOP_LEFT, 20, 12);

    // 行图标 1:OrangePi 板载芯片(橙方块+引脚)
    lv_obj_t* chip = lv_obj_create(rows_[1]);
    lv_obj_set_size(chip, 28, 22);
    lv_obj_set_style_bg_color(chip, lv_color_hex(0xf5a263), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_radius(chip, 2, 0);
    lv_obj_set_style_pad_all(chip, 0, 0);
    lv_obj_set_scrollbar_mode(chip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(chip, 16, 20);
    for (int p = 0; p < 4; p++) {
        lv_obj_t* pin = lv_obj_create(rows_[1]);
        lv_obj_set_size(pin, 2, 6);
        lv_obj_set_style_bg_color(pin, lv_color_hex(0xb06a3a), 0);
        lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pin, 0, 0);
        lv_obj_set_style_radius(pin, 0, 0);
        lv_obj_set_style_pad_all(pin, 0, 0);
        lv_obj_set_scrollbar_mode(pin, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(pin, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(pin, 19 + p * 7, 14);
        lv_obj_t* pin2 = lv_obj_create(rows_[1]);
        lv_obj_set_size(pin2, 2, 6);
        lv_obj_set_style_bg_color(pin2, lv_color_hex(0xb06a3a), 0);
        lv_obj_set_style_bg_opa(pin2, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pin2, 0, 0);
        lv_obj_set_style_radius(pin2, 0, 0);
        lv_obj_set_style_pad_all(pin2, 0, 0);
        lv_obj_set_scrollbar_mode(pin2, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(pin2, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(pin2, 19 + p * 7, 42);
    }

    // 底部按键提示
    lv_obj_t* hint = lv_label_create(root_);
    lv_obj_set_style_text_font(hint, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_label_set_text(hint, "OK 进入 · 长按返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void ToolsApp::Highlight() {
    for (int i = 0; i < kToolCount; i++) {
        if (rows_[i] == nullptr) continue;
        lv_obj_set_style_border_color(rows_[i],
            lv_color_hex(i == sel_ ? C_FG : C_DIM), 0);
        lv_obj_set_style_border_width(rows_[i],
            i == sel_ ? 2 : 1, 0);
    }
}

void ToolsApp::HandleKey(int key) {
    if (!active_ || root_ == nullptr) return;
    switch (key) {
        case 0:   // 上:上一项(循环)
            sel_ = (sel_ + kToolCount - 1) % kToolCount;
            Highlight();
            break;
        case 1:   // 下:下一项(循环)
            sel_ = (sel_ + 1) % kToolCount;
            Highlight();
            break;
        case 2:   // OK:进入选中工具(SwitchApp 会销毁本文件夹)
            HomeScreen::Get().SwitchApp(sel_ == 0 ? 9 : 12);   // 9=新闻 12=OrangePi
            break;
        default:
            break;
    }
}

void ToolsApp::Enter() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && !active_) {
            lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
            active_ = true;
            Highlight();
        }
        lvgl_port_unlock();
    }
}

void ToolsApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        lvgl_port_unlock();
    }
}

void ToolsApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && active_) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            active_ = false;
        }
        lvgl_port_unlock();
    }
}
