// games_app.cc —— 娱乐文件夹选择屏实现(方块/Flappy/答案之书)。
#include "games_app.h"

LV_FONT_DECLARE(font_home_cn_16);

#include "settings.h"
#include "home_screen.h"

#include <cstdio>

#include "esp_lvgl_port.h"

#define C_BG 0x0a1f14
#define C_FG 0xb7f5c4
#define C_DIM 0x4f7a5e
#define C_HINT 0x34573f

GamesApp& GamesApp::Get() {
    static GamesApp instance;
    return instance;
}

// 小矩形工具(与 flappy_app 同款,可选圆角)
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

void GamesApp::Create(lv_obj_t* parent) {
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
    lv_label_set_text(title, "娱乐");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    // 三个条目行(图标左,名称+副题右)
    static const char* kNames[kGameCount] = {"俄罗斯方块", "FLAPPY BIRD", "答案之书"};
    for (int i = 0; i < kGameCount; i++) {
        lv_obj_t* row = lv_obj_create(root_);
        lv_obj_set_size(row, 208, 60);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x081710), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(C_DIM), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 64 + i * 72);
        rows_[i] = row;

        lv_obj_t* name = lv_label_create(row);
        lv_obj_set_style_text_font(name, &font_home_cn_16, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_FG), 0);
        lv_label_set_text(name, kNames[i]);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 64, 7);

        lv_obj_t* hi = lv_label_create(row);
        lv_obj_set_style_text_font(hi, &font_home_cn_16, 0);
        lv_obj_set_style_text_color(hi, lv_color_hex(C_DIM), 0);
        lv_label_set_text(hi, "最高 --");
        lv_obj_align(hi, LV_ALIGN_TOP_LEFT, 64, 33);
        score_labels_[i] = hi;
    }

    // 行图标 0:方块 L 形(与主页原方块图标同款,橙)
    static const int lshape[4][2] = {{0, 0}, {1, 0}, {1, 1}, {1, 2}};
    for (int i = 0; i < 4; i++) {
        lv_obj_t* b = mk_rect(rows_[0], 10, 10, 0xf5a263);
        lv_obj_set_pos(b, 14 + lshape[i][1] * 12, 12 + lshape[i][0] * 12);
    }
    // 行图标 1:像素鸟(体黄/翅白/喙橙)
    lv_obj_t* body = mk_rect(rows_[1], 28, 22, 0xf5e663);
    lv_obj_set_pos(body, 14, 21);
    lv_obj_t* wing = mk_rect(rows_[1], 12, 6, 0xfdf0c8);
    lv_obj_set_pos(wing, 17, 31);
    lv_obj_t* beak = mk_rect(rows_[1], 7, 6, 0xf5a263);
    lv_obj_set_pos(beak, 40, 29);
    lv_obj_t* eye = mk_rect(rows_[1], 4, 4, 0x0a1f14);
    lv_obj_set_pos(eye, 34, 26);
    // 行图标 2:答案之书 "书"字(与原启动栏图标同款,青色)
    lv_obj_t* book = lv_label_create(rows_[2]);
    lv_obj_set_style_text_font(book, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(book, lv_color_hex(0x8ae0f7), 0);
    lv_label_set_text(book, "书");
    lv_obj_set_pos(book, 24, 22);

    // 底部按键提示
    lv_obj_t* hint = lv_label_create(root_);
    lv_obj_set_style_text_font(hint, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_label_set_text(hint, "OK 进入 · 长按返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void GamesApp::RefreshScores() {
    int hi;
    {   Settings st("tetris", false); hi = st.GetInt("highscore", 0); }
    lv_label_set_text_fmt(score_labels_[0], "最高 %d", hi);
    {   Settings st("flappy", false); hi = st.GetInt("highscore", 0); }
    lv_label_set_text_fmt(score_labels_[1], "最高 %d", hi);
    lv_label_set_text(score_labels_[2], "随机一句答案");
}

void GamesApp::Highlight() {
    for (int i = 0; i < kGameCount; i++) {
        if (rows_[i] == nullptr) continue;
        lv_obj_set_style_border_color(rows_[i],
            lv_color_hex(i == sel_ ? C_FG : C_DIM), 0);
        lv_obj_set_style_border_width(rows_[i],
            i == sel_ ? 2 : 1, 0);
    }
}

void GamesApp::HandleKey(int key) {
    if (!active_ || root_ == nullptr) return;
    switch (key) {
        case 0:   // 上:上一项(循环)
            sel_ = (sel_ + kGameCount - 1) % kGameCount;
            Highlight();
            break;
        case 1:   // 下:下一项(循环)
            sel_ = (sel_ + 1) % kGameCount;
            Highlight();
            break;
        case 2:   // OK:进入选中项(SwitchApp 会销毁本文件夹)
            HomeScreen::Get().SwitchApp(sel_ == 2 ? 2 : 6 + sel_);   // 6=方块 7=Flappy 2=答案之书
            break;
        default:
            break;
    }
}

void GamesApp::Enter() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && !active_) {
            lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
            active_ = true;
            RefreshScores();
            Highlight();
        }
        lvgl_port_unlock();
    }
}

void GamesApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        lvgl_port_unlock();
    }
}

void GamesApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && active_) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            active_ = false;
        }
        lvgl_port_unlock();
    }
}
