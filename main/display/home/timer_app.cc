// timer_app —— 倒计时器(预设网格 + 到时连响 5 声)。
// 见 timer_app.h 按键说明。

#include "timer_app.h"

#include "timer_beep.h"

#include <cstdio>

#include "esp_log.h"
#include "application.h"
#include "board.h"
#include "assets/lang_config.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

LV_FONT_DECLARE(font_home_cn_16);

static const char* TAG = "timer";

TimerApp& TimerApp::Get() {
    static TimerApp inst;
    return inst;
}

void TimerApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_center(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 6, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // ---------- 选择页 ----------
        

    // 2 行 × 3 列按钮网格(240 宽:按钮 68×44,间距 6)
    static const char* names[kPresets] = {
        "00:30", "1:00", "2:00", "5:00", "8:00", "10:00"};
    for (int i = 0; i < kPresets; i++) {
        int row = i / 3, col = i % 3;
        int x = 16 + col * 72;
        int y = 104 + row * 54;
        sel_btn_[i] = lv_btn_create(root_);
        lv_obj_set_size(sel_btn_[i], 68, 44);
        lv_obj_set_pos(sel_btn_[i], x, y);
        lv_obj_set_style_radius(sel_btn_[i], 8, 0);
        lv_obj_set_style_bg_color(sel_btn_[i], lv_color_hex(0x1A2129), 0);
        lv_obj_set_style_border_width(sel_btn_[i], 2, 0);
        lv_obj_set_style_border_color(sel_btn_[i], lv_color_hex(0x2A333D), 0);
        lv_obj_set_style_shadow_width(sel_btn_[i], 0, 0);
        sel_lbl_[i] = lv_label_create(sel_btn_[i]);
        lv_obj_set_style_text_font(sel_lbl_[i], &lv_font_montserrat_16, 0);
        lv_obj_center(sel_lbl_[i]);
        lv_label_set_text(sel_lbl_[i], names[i]);
    }

    status_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(status_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0xAAB4BE), 0);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, "");
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 224);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_label_, 224);

    // ---------- 倒计时页 ----------
    mm_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(mm_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(mm_label_, lv_color_hex(0x9FE870), 0);
    lv_label_set_text(mm_label_, "05:");
    lv_obj_align(mm_label_, LV_ALIGN_TOP_MID, -32, 22);
    ss_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(ss_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(ss_label_, lv_color_hex(0x9FE870), 0);
    lv_label_set_text(ss_label_, "00");
    lv_obj_align_to(ss_label_, mm_label_, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

    mm_line_ = lv_obj_create(root_);
    lv_obj_set_size(mm_line_, 58, 3);
    lv_obj_set_style_bg_color(mm_line_, lv_color_hex(0xFFC24B), 0);
    lv_obj_set_style_bg_opa(mm_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mm_line_, 0, 0);
    lv_obj_set_style_radius(mm_line_, 0, 0);
    lv_obj_align_to(mm_line_, mm_label_, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_add_flag(mm_line_, LV_OBJ_FLAG_HIDDEN);

    ss_line_ = lv_obj_create(root_);
    lv_obj_set_size(ss_line_, 58, 3);
    lv_obj_set_style_bg_color(ss_line_, lv_color_hex(0xFFC24B), 0);
    lv_obj_set_style_bg_opa(ss_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ss_line_, 0, 0);
    lv_obj_set_style_radius(ss_line_, 0, 0);
    lv_obj_align_to(ss_line_, ss_label_, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_add_flag(ss_line_, LV_OBJ_FLAG_HIDDEN);

    field_hint_ = lv_label_create(root_);
    lv_obj_set_style_text_font(field_hint_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(field_hint_, lv_color_hex(0xFFC24B), 0);
    lv_label_set_text(field_hint_, "");
    lv_obj_align(field_hint_, LV_ALIGN_TOP_MID, 0, 128);

    bar_ = lv_bar_create(root_);
    lv_obj_set_size(bar_, 208, 10);
    lv_obj_align(bar_, LV_ALIGN_TOP_MID, 0, 158);
    lv_bar_set_range(bar_, 0, 100);
    lv_bar_set_value(bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_, lv_color_hex(0x222A31), 0);
    lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, 0);
    {
        static lv_style_t ind;
        static bool inited = false;
        if (!inited) {
            lv_style_init(&ind);
            lv_style_set_bg_color(&ind, lv_color_hex(0x9FE870));
            lv_style_set_bg_opa(&ind, LV_OPA_COVER);
            lv_style_set_radius(&ind, 4);
            inited = true;
        }
        lv_obj_add_style(bar_, &ind, LV_PART_INDICATOR);
    }

    // 500ms tick:RUNNING 递减 + 播完恢复音量
    timer_ = lv_timer_create(TimerApp::TickCb, 1000, this);   // 1s/次
    lv_timer_pause(timer_);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void TimerApp::TickCb(lv_timer_t* t) {
    static_cast<TimerApp*>(lv_timer_get_user_data(t))->Tick();
}

void TimerApp::Tick() {
    // DONE 态:背景黄/暗交替闪烁(无声也有可靠视觉提醒)
    if (st_ == St::DONE && root_ != nullptr) {
        static bool blink = false;
        blink = !blink;
        lv_obj_set_style_bg_color(root_,
            blink ? lv_color_hex(0x3A2E10) : lv_color_hex(0x101418), 0);
    }
    if (st_ == St::RUNNING) {
        remain_--;
        if (remain_ <= 0) {
            remain_ = 0;
            st_ = St::DONE;
            RenderBig();
            // 到时蜂鸣:直接写 I2S 的独立任务(绕过音频服务管线,零锁零队列)
            StartTimerBeepTask();
        } else {
            RenderBig();
        }
    }
}

void TimerApp::HandleKey(int key) {
    switch (st_) {
        case St::SEL:
            if (root_ != nullptr) lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
            if (key == 0) {          // 上=上一个(环绕)
                sel_ = (sel_ + kPresets - 1) % kPresets;
                RenderSel();
            } else if (key == 1) {   // 下=下一个(环绕)
                sel_ = (sel_ + 1) % kPresets;
                RenderSel();
            } else if (key == 2) {   // OK=开始
                total_ = preset_s_[sel_];
                if (total_ <= 0) return;
                remain_ = total_;
                st_ = St::RUNNING;
                Render();
            } else if (key == 4) {   // 长按上=编辑选中值
                st_ = St::EDITING;
                set_min_ = preset_s_[sel_] / 60;
                set_sec_ = preset_s_[sel_] % 60;
                edit_field_min_ = true;
                Render();
            }
            return;

        case St::EDITING:
            if (key == 0) {          // 上=+1
                if (edit_field_min_) set_min_ = (set_min_ + 1) % 100;
                else set_sec_ = (set_sec_ + 1) % 60;
                RenderBig();
            } else if (key == 1) {   // 下=-1
                if (edit_field_min_) set_min_ = (set_min_ + 99) % 100;
                else set_sec_ = (set_sec_ + 59) % 60;
                RenderBig();
            } else if (key == 5) {   // 长按下=切换分/秒
                edit_field_min_ = !edit_field_min_;
                RenderBig();
            } else if (key == 2) {   // OK=用编辑值直接开始(预设不变)
                total_ = set_min_ * 60 + set_sec_;
                if (total_ <= 0) total_ = 300;
                remain_ = total_;
                st_ = St::RUNNING;
                Render();
            }
            return;

        case St::RUNNING:
            if (key == 2) {
                st_ = St::PAUSED;
                RenderBig();
            }
            return;

        case St::PAUSED:
            if (key == 2) {
                st_ = St::RUNNING;
                RenderBig();
            }
            return;

        case St::DONE:
            if (key == 2) {          // OK=回选择页
                st_ = St::SEL;
                if (root_ != nullptr) lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
                Render();
            }
            return;
    }
}

void TimerApp::Render() {
    // 选择页 vs 倒计时页 显示切换
    bool sel_page = (st_ == St::SEL);
    for (int i = 0; i < kPresets; i++) {
        if (sel_page) lv_obj_clear_flag(sel_btn_[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(sel_btn_[i], LV_OBJ_FLAG_HIDDEN);
    }
    // 大字两页共用(选择页=选中值,倒计时页=剩余)
    // 进度条仅倒计时页
    if (sel_page) lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    if (sel_page) {
        RenderSel();
    } else {
        RenderBig();
    }
}

void TimerApp::RenderSel() {
    // 大字显示选中预设值
    int secs = preset_s_[sel_];
    lv_label_set_text_fmt(mm_label_, "%02d:", secs / 60);
    lv_label_set_text_fmt(ss_label_, "%02d", secs % 60);
    lv_obj_set_style_text_color(mm_label_, lv_color_hex(0x9FE870), 0);
    lv_obj_set_style_text_color(ss_label_, lv_color_hex(0x9FE870), 0);
    // 清理编辑态残留(下划线/字段提示)
    lv_obj_add_flag(mm_line_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ss_line_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(field_hint_, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < kPresets; i++) {
        bool on = (i == sel_);
        lv_obj_set_style_border_color(sel_btn_[i],
            on ? lv_color_hex(0xFFC24B) : lv_color_hex(0x2A333D), 0);
        lv_obj_set_style_bg_color(sel_btn_[i],
            on ? lv_color_hex(0x2B3320) : lv_color_hex(0x1A2129), 0);
        lv_obj_set_style_text_color(sel_lbl_[i],
            on ? lv_color_hex(0xFFC24B) : lv_color_hex(0x8A949E), 0);
        int secs = preset_s_[i];
        lv_label_set_text_fmt(sel_lbl_[i], "%d:%02d", secs / 60, secs % 60);
    }
    lv_label_set_text(status_label_,
        "上/下:选择 OK:开始\n长按上:修改数值 长按OK:退出");
}

void TimerApp::RenderBig() {
    int mm = (st_ == St::EDITING) ? set_min_ : remain_ / 60;
    int ss = (st_ == St::EDITING) ? set_sec_ : remain_ % 60;

    lv_label_set_text_fmt(mm_label_, "%02d:", mm);
    lv_label_set_text_fmt(ss_label_, "%02d", ss);

    lv_color_t c = st_ == St::DONE ? lv_color_hex(0xFFC24B)
                : st_ == St::PAUSED ? lv_color_hex(0xE8E3D3)
                : lv_color_hex(0x9FE870);
    lv_obj_set_style_text_color(mm_label_, c, 0);
    lv_obj_set_style_text_color(ss_label_, c, 0);

    if (st_ == St::EDITING) {
        lv_obj_add_flag(edit_field_min_ ? ss_line_ : mm_line_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(edit_field_min_ ? mm_line_ : ss_line_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(field_hint_, edit_field_min_ ? "编辑[分]" : "编辑[秒]");
        lv_obj_clear_flag(field_hint_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(mm_line_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ss_line_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(field_hint_, LV_OBJ_FLAG_HIDDEN);
    }

    switch (st_) {
        case St::EDITING:
            lv_label_set_text(status_label_,
                "上/下:数字加/减 长按下:切换分秒\nOK:保存");
            break;
        case St::RUNNING:
            lv_label_set_text(status_label_,
                "计时中…\nOK:暂停 长按OK:退出");
            break;
        case St::PAUSED:
            lv_label_set_text(status_label_,
                "已暂停\nOK:继续 长按OK:退出");
            break;
        case St::DONE:
            lv_label_set_text(status_label_,
                "时间到!\nOK:返回选择");
            break;
        default: break;
    }

    if (st_ == St::RUNNING || st_ == St::PAUSED) {
        int pct = total_ > 0 ? (int)((long long)(total_ - remain_) * 100 / total_) : 0;
        lv_bar_set_value(bar_, pct, LV_ANIM_OFF);
        lv_obj_clear_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    } else if (st_ == St::DONE) {
        lv_bar_set_value(bar_, 100, LV_ANIM_OFF);
        lv_obj_clear_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void TimerApp::Enter() {
    if (root_ == nullptr) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
        active_ = true;
        if (timer_ != nullptr) lv_timer_resume(timer_);
        st_ = St::SEL;
        Render();
        lvgl_port_unlock();
    }
}

void TimerApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        lvgl_port_unlock();
    }
}

void TimerApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (timer_ != nullptr) lv_timer_pause(timer_);
        active_ = false;
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}
