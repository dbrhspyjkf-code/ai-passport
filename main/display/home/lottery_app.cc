// lottery_app —— 双色球开奖查看器实现(边框卡片+红蓝分色)。
#include "lottery_app.h"

#include <cstdio>
#include <cstring>

#include "esp_lvgl_port.h"

#include "lottery_service.h"
#include "theme.h"

LV_FONT_DECLARE(font_home_cn_16);

LotteryApp& LotteryApp::Get() {
    static LotteryApp inst;
    return inst;
}

static void fmt_yi(long val, char* buf, int cap) {
    if (val >= 100000000) snprintf(buf, cap, "%.1f亿", (double)val / 1e8);
    else if (val >= 10000) snprintf(buf, cap, "%.0f万", (double)val / 1e4);
    else snprintf(buf, cap, "%ld", val);
}

void LotteryApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(theme::bg()), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 8, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    title_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(title_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_hex(theme::fg()), 0);
    lv_label_set_text(title_label_, "双色球");
    lv_obj_align(title_label_, LV_ALIGN_TOP_LEFT, 0, 2);

    // 3 期卡片, 每期 88px 高(y=28, 120, 212)
    for (int i = 0; i < kPerPage; i++) {
        int y = 28 + i * 84;

        // 卡片容器(圆角边框)
        cards_[i] = lv_obj_create(root_);
        lv_obj_set_size(cards_[i], LV_HOR_RES - 16, 76);
        lv_obj_set_style_bg_color(cards_[i], lv_color_hex(theme::bg()), 0);
        lv_obj_set_style_bg_opa(cards_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(cards_[i], lv_color_hex(theme::dim()), 0);
        lv_obj_set_style_border_width(cards_[i], 1, 0);
        lv_obj_set_style_border_opa(cards_[i], LV_OPA_60, 0);
        lv_obj_set_style_radius(cards_[i], 6, 0);
        lv_obj_set_style_pad_all(cards_[i], 4, 0);
        lv_obj_set_scrollbar_mode(cards_[i], LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(cards_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(cards_[i], 0, y);

        // 期号行(居中)
        hdr_lbl_[i] = lv_label_create(cards_[i]);
        lv_obj_set_style_text_font(hdr_lbl_[i], &font_home_cn_16, 0);
        lv_obj_set_style_text_color(hdr_lbl_[i], lv_color_hex(theme::fg()), 0);
        lv_label_set_text(hdr_lbl_[i], "");
        lv_obj_align(hdr_lbl_[i], LV_ALIGN_TOP_MID, 0, 2);

        // 红蓝球行(recolor 分色:红 #FF4444 蓝 #4488FF)
        ball_lbl_[i] = lv_label_create(cards_[i]);
        lv_obj_set_style_text_font(ball_lbl_[i], &font_home_cn_16, 0);
        lv_label_set_recolor(ball_lbl_[i], true);
        lv_label_set_text(ball_lbl_[i], "");
        lv_obj_align(ball_lbl_[i], LV_ALIGN_TOP_MID, 0, 22);

        // 信息行(销售/奖池/一等奖)
        info_lbl_[i] = lv_label_create(cards_[i]);
        lv_obj_set_style_text_font(info_lbl_[i], &font_home_cn_16, 0);
        lv_obj_set_style_text_color(info_lbl_[i], lv_color_hex(theme::dim()), 0);
        lv_label_set_text(info_lbl_[i], "");
        lv_obj_align(info_lbl_[i], LV_ALIGN_TOP_MID, 0, 46);
    }

    lv_obj_t* hint = lv_label_create(root_);
    lv_obj_set_style_text_font(hint, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(theme::dim()), 0);
    lv_label_set_text(hint, "上下翻页 OK刷新");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    state_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(state_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(state_label_, lv_color_hex(theme::dim()), 0);
    lv_label_set_text(state_label_, "");
    lv_obj_align(state_label_, LV_ALIGN_TOP_MID, 0, 140);

    sync_timer_ = lv_timer_create([](lv_timer_t* t) {
        static_cast<LotteryApp*>(lv_timer_get_user_data(t))->SyncFromCache();
    }, 500, this);
    lv_timer_pause(sync_timer_);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void LotteryApp::Render() {
    if (root_ == nullptr) return;
    auto& ls = LotteryService::Get();
    if (!ls.Valid() || ls.Count() == 0) {
        for (int i = 0; i < kPerPage; i++)
            lv_label_set_text(hdr_lbl_[i], "");
        lv_label_set_text(state_label_, "暂无数据");
        return;
    }
    lv_label_set_text(state_label_, "");

    int start = page_ * kPerPage;
    int total = ls.Count();
    if (start >= total) { start = 0; page_ = 0; }

    for (int i = 0; i < kPerPage; i++) {
        int idx = start + i;
        if (idx >= total) {
            lv_obj_add_flag(cards_[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(cards_[i], LV_OBJ_FLAG_HIDDEN);
        const LotteryService::Issue& is = ls.Get(idx);
        const char* sc = is.code;
        int cl = strlen(is.code);
        if (cl > 3) sc = is.code + (cl - 3);

        // 期号行(居中)
        char hb[32];
        snprintf(hb, sizeof(hb), "%s期  %s", sc, is.date);
        lv_label_set_text(hdr_lbl_[i], hb);

        // 红蓝球行(recolor: #FF4444 红球 #4488FF 蓝球)
        char bb[96];
        snprintf(bb, sizeof(bb),
            "#FF5050 %02d %02d %02d %02d %02d %02d#  "
            "#4488FF + %02d#",
            is.reds[0], is.reds[1], is.reds[2],
            is.reds[3], is.reds[4], is.reds[5], is.blue);
        lv_label_set_text(ball_lbl_[i], bb);

        // 信息行
        char sales[16], pool[16];
        fmt_yi(is.sales, sales, sizeof(sales));
        fmt_yi(is.pool, pool, sizeof(pool));
        char ib[64];
        snprintf(ib, sizeof(ib), "销售%s 奖池%s 一等%d注",
                 sales, pool, is.prize1_count);
        lv_label_set_text(info_lbl_[i], ib);
    }

    int tp = (total + kPerPage - 1) / kPerPage;
    char ps[48];
    snprintf(ps, sizeof(ps), "双色球 %d/%d页", page_ + 1, tp);
    lv_label_set_text(title_label_, ps);
}

void LotteryApp::SyncFromCache() {
    if (!active_) return;
    const uint32_t v = LotteryService::Get().Version();
    if (v == seen_version_) return;
    seen_version_ = v;
    page_ = 0;
    Render();
}

void LotteryApp::HandleKey(int key) {
    if (!active_) return;
    auto& ls = LotteryService::Get();
    int tp = (ls.Count() + kPerPage - 1) / kPerPage;
    if (tp < 1) tp = 1;
    switch (key) {
        case 0: page_ = (page_ + tp - 1) % tp; Render(); break;
        case 1: page_ = (page_ + 1) % tp; Render(); break;
        case 2:
            LotteryService::Get().RequestNow();
            lv_label_set_text(state_label_, "获取中…");
            break;
    }
}

void LotteryApp::Enter() {
    if (root_ == nullptr) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
        active_ = true;
        page_ = 0;
        seen_version_ = LotteryService::Get().Version();
        Render();
        LotteryService::Get().RequestNow();
        if (!LotteryService::Get().Valid()) lv_label_set_text(state_label_, "获取中…");
        if (sync_timer_ != nullptr) lv_timer_resume(sync_timer_);
        lvgl_port_unlock();
    }
}

void LotteryApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        active_ = false;
        if (sync_timer_ != nullptr) lv_timer_pause(sync_timer_);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

void LotteryApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) { lv_obj_delete(root_); root_ = nullptr; }
        if (sync_timer_ != nullptr) { lv_timer_delete(sync_timer_); sync_timer_ = nullptr; }
        lvgl_port_unlock();
    }
}
