#pragma once

// timer_app —— 倒计时器。
// SEL:      6 个预设两行网格,上/下=选择,OK=开始;长按上=编辑选中值
// EDITING:  上/下=当前字段±1,长按下=切换分/秒,OK=保存回选择
// RUNNING:  OK=暂停
// PAUSED:   OK=继续
// DONE:     OK=回选择
// 长按OK=退出APP(board层)
// 到时:连续 5 声提示音(临时满音量,播完恢复)

#include "lvgl.h"

class TimerApp {
public:
    static TimerApp& Get();

    void Create(lv_obj_t* parent);
    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK_CLICK 4=UP_LONG 5=DOWN_LONG

private:
    TimerApp() = default;

    enum class St { SEL, EDITING, RUNNING, PAUSED, DONE };
    static constexpr int kPresets = 6;

    void Render();
    void RenderSel();
    void RenderBig();
    static void TickCb(lv_timer_t* t);
    void Tick();

    lv_obj_t* root_ = nullptr;
    // 选择页
    lv_obj_t* sel_btn_[kPresets] = {};
    lv_obj_t* sel_lbl_[kPresets] = {};
    // 倒计时页
    lv_obj_t* mm_label_ = nullptr;
    lv_obj_t* ss_label_ = nullptr;
    lv_obj_t* mm_line_ = nullptr;
    lv_obj_t* ss_line_ = nullptr;
    lv_obj_t* field_hint_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* bar_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    St st_ = St::SEL;
    int preset_s_[kPresets] = {30, 60, 120, 300, 480, 600};   // 预设(秒)
    int sel_ = 3;              // 选中索引(默认 5:00)
    int set_min_ = 0;          // 编辑缓冲
    int set_sec_ = 0;
    bool edit_field_min_ = true;
    int total_ = 300;
    int remain_ = 0;
    int saved_vol_ = -1;
    uint32_t restore_tick_ = 0;
    int beep_count_ = 0;         // 已播声数(0=未开始,>0 序列中)
    uint32_t next_beep_tick_ = 0;
    bool active_ = false;
};
