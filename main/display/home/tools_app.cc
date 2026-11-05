// tools_app.cc —— 工具文件夹选择屏实现(新闻/OrangePi)。
#include "theme.h"
#include "tools_app.h"

LV_FONT_DECLARE(font_home_cn_16);

#include "home_screen.h"

#include <cstdio>

#include "esp_lvgl_port.h"

// 深蓝工具风(区别于娱乐文件夹的墨绿)
#define C_BG theme::bg()
#define C_FG theme::fg()
#define C_DIM theme::dim()
#define C_HINT theme::hint()

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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    // 三个条目行(图标左,名称+说明右)
    static const char* kNames[kToolCount] = {"ORANGEPI", "健康", "双色球"};
    static const char* kSubs[kToolCount] = {"系统状态监控", "iPhone 健康数据", "近 16 期开奖"};
    for (int i = 0; i < kToolCount; i++) {
        lv_obj_t* row = lv_obj_create(root_);
        lv_obj_set_size(row, 208, 52);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x081120), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 6, 0);   // 轻微圆角(与娱乐文件夹一致)
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 40 + i * 64);
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

    // 行图标 1:OrangePi 板载芯片(橙方块+引脚)
    lv_obj_t* chip = lv_obj_create(rows_[0]);
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
        lv_obj_t* pin = lv_obj_create(rows_[0]);
        lv_obj_set_size(pin, 2, 6);
        lv_obj_set_style_bg_color(pin, lv_color_hex(0xb06a3a), 0);
        lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pin, 0, 0);
        lv_obj_set_style_radius(pin, 0, 0);
        lv_obj_set_style_pad_all(pin, 0, 0);
        lv_obj_set_scrollbar_mode(pin, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(pin, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(pin, 19 + p * 7, 14);
        lv_obj_t* pin2 = lv_obj_create(rows_[0]);
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

    // 行图标 2:健康心形(红,两圆角块+旋转方块拼合)
    {
        auto mk = [](lv_obj_t* parent, int w, int h, uint32_t color, int x, int y) {
            lv_obj_t* r = lv_obj_create(parent);
            lv_obj_set_size(r, w, h);
            lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(r, 0, 0);
            lv_obj_set_style_radius(r, 5, 0);
            lv_obj_set_style_pad_all(r, 0, 0);
            lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(r, x, y);
        };
        mk(rows_[2], 14, 12, 0xFF3B30, 18, 20);    // 左半圆
        mk(rows_[2], 14, 12, 0xFF3B30, 32, 20);    // 右半圆
        mk(rows_[2], 26, 14, 0xFF3B30, 19, 28);    // 下部方块
    }

    // 行图标 3:双色球(红圆+蓝圆重叠,彩票风)
    {
        auto mk = [](lv_obj_t* parent, int w, int h, uint32_t color, int x, int y, int r) {
            lv_obj_t* o = lv_obj_create(parent);
            lv_obj_set_size(o, w, h);
            lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(o, 0, 0);
            lv_obj_set_style_radius(o, r, 0);
            lv_obj_set_style_pad_all(o, 0, 0);
            lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(o, x, y);
        };
        // 红球(大,左)
        mk(rows_[3], 22, 22, 0xFF5050, 18, 18, 11);
        // 蓝球(小,右下,部分重叠)
        mk(rows_[3], 16, 16, 0x4488FF, 32, 26, 8);
        // 蓝球上的白点装饰
        mk(rows_[3], 4, 4, 0xE8E3D3, 38, 32, 2);
    }

    // 底部按键提示
    lv_obj_t* hint = lv_label_create(root_);
    lv_obj_set_style_text_font(hint, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_label_set_text(hint, "OK 进入 · 长按返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

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
        case 2: { // OK:进入选中工具(SwitchApp 会销毁本文件夹)
            int idx = sel_ == 0 ? 12 : sel_ == 1 ? 13 : 14;
            HomeScreen::Get().SwitchApp(idx);
            break;
        }
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
