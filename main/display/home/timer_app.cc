// timer_app —— 倒计时器(极简单页版)。见 timer_app.h。

#include "timer_app.h"

#include "timer_beep.h"
#include "application.h"
#include "assets/lang_config.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_lvgl_port.h"

LV_FONT_DECLARE(font_home_cn_16);

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
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部大字(选时=预设 / 运行=倒计时)
    big_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(big_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(big_label_, lv_color_hex(0x9FE870), 0);
    lv_label_set_text(big_label_, "05:00");
    lv_obj_align(big_label_, LV_ALIGN_TOP_MID, 0, 26);

    // 6 格预设(两行三列)
    static const char* names[kPresets] = {
        "00:30", "1:00", "2:00", "5:00", "8:00", "10:00"};
    for (int i = 0; i < kPresets; i++) {
        int row = i / 3, col = i % 3;
        sel_btn_[i] = lv_btn_create(root_);
        lv_obj_set_size(sel_btn_[i], 68, 44);
        lv_obj_set_pos(sel_btn_[i], 16 + col * 72, 110 + row * 54);
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

    // 底部提示
    status_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(status_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0xAAB4BE), 0);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 236);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_label_, 224);

    // 1 秒 tick(倒计时推进)
    timer_ = lv_timer_create(TimerApp::TickCb, 1000, this);
    lv_timer_pause(timer_);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void TimerApp::TickCb(lv_timer_t* t) {
    static_cast<TimerApp*>(lv_timer_get_user_data(t))->Tick();
}

void TimerApp::Tick() {
    if (st_ == St::DONE && root_ != nullptr) {
        // 提醒节奏:第 1-3 秒每秒嘀一声(main 直播,已验证路径),
        // 第 4-9 秒闪屏 3 次(亮暗交替),之后停在暗底。
        done_ticks_++;
        if (done_ticks_ >= 1 && done_ticks_ <= 3) {
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().GetAudioService().PlaySound(
                    Lang::Sounds::OGG_CHIME_FULL);
            });
        } else if (done_ticks_ >= 4 && done_ticks_ <= 9) {
            // tick4亮 5暗 6亮 7暗 8亮 9暗 = 3 次完整闪烁
            bool lit = ((done_ticks_ - 4) % 2) == 0;
            lv_obj_set_style_bg_color(root_,
                lit ? lv_color_hex(0x3A2E10) : lv_color_hex(0x101418), 0);
        }
        return;
    }
    if (st_ != St::RUNNING) return;
    remain_--;
    // 剩 3 秒:播极短静音预热音频栈(重置 power timer 15s 窗口,
    // 到时时 output 仍开、管线热 —— 冷态播放会无声/死机)
    if (remain_ == 3) {
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().GetAudioService().PlaySound(
                Lang::Sounds::OGG_SILENCE0);
        });
    }
    if (remain_ <= 0) {
        remain_ = 0;
        st_ = St::DONE;
        done_ticks_ = 0;
        Render();
        // 到时提醒桥接(当前策略:闪屏,声音停用——见 application.cc 注释)
        StartTimerBeepTask();
        return;
    }
    Render();
}

void TimerApp::HandleKey(int key) {
    switch (st_) {
        case St::SEL:
            if (key == 0) { sel_ = (sel_ + kPresets - 1) % kPresets; Render(); }
            else if (key == 1) { sel_ = (sel_ + 1) % kPresets; Render(); }
            else if (key == 2) {
                total_ = preset_s_[sel_];
                if (total_ <= 0) return;
                remain_ = total_;
                st_ = St::RUNNING;
                Render();
            }
            return;
        case St::RUNNING:
            if (key == 2) { st_ = St::PAUSED; Render(); }
            return;
        case St::PAUSED:
            if (key == 2) { st_ = St::RUNNING; Render(); }
            return;
        case St::DONE:
            if (key == 2) {
                st_ = St::SEL;
                if (root_ != nullptr) lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
                Render();
            }
            return;
    }
}

void TimerApp::Render() {
    if (root_ == nullptr) return;

    // 大字:待机=预设,运行/暂停=剩余,完成=00:00
    int show = (st_ == St::SEL) ? preset_s_[sel_] : remain_;
    lv_label_set_text_fmt(big_label_, "%02d:%02d", show / 60, show % 60);
    lv_obj_set_style_text_color(big_label_,
        st_ == St::DONE ? lv_color_hex(0xFFC24B)
        : st_ == St::PAUSED ? lv_color_hex(0xE8E3D3)
        : lv_color_hex(0x9FE870), 0);

    // 网格高亮(仅待机可选)
    for (int i = 0; i < kPresets; i++) {
        bool on = (i == sel_) && (st_ == St::SEL);
        lv_obj_set_style_border_color(sel_btn_[i],
            on ? lv_color_hex(0xFFC24B) : lv_color_hex(0x2A333D), 0);
        lv_obj_set_style_bg_color(sel_btn_[i],
            on ? lv_color_hex(0x2B3320) : lv_color_hex(0x1A2129), 0);
        lv_obj_set_style_text_color(sel_lbl_[i],
            on ? lv_color_hex(0xFFC24B) : lv_color_hex(0x8A949E), 0);
    }

    // 提示行
    switch (st_) {
        case St::SEL:
            lv_label_set_text(status_label_, "上下选时长 OK 开始");
            break;
        case St::RUNNING:
            lv_label_set_text(status_label_, "OK 暂停 · 长按OK 退出");
            break;
        case St::PAUSED:
            lv_label_set_text(status_label_, "已暂停 · OK 继续");
            break;
        case St::DONE:
            lv_label_set_text(status_label_, "时间到! OK 返回");
            break;
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
        done_ticks_ = 0;
        lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
        Render();
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
