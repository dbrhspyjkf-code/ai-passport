#pragma once

// timer_app —— 倒计时器(极简单页版)。
// 唯一界面:顶部大字 + 预设网格 + 底部提示。
//   大字: 选时=预设值 / 运行=倒计时 / 结束=00:00(背景闪烁)
// 按键: 上/下=选预设(仅待机) OK=开始/暂停/继续/返回
//       长按 OK=退出 APP(board 层)

#include "lvgl.h"

class TimerApp {
public:
    static TimerApp& Get();

    void Create(lv_obj_t* parent);
    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK_CLICK

private:
    TimerApp() = default;

    enum class St { SEL, RUNNING, PAUSED, DONE };
    static constexpr int kPresets = 6;

    void Render();
    static void TickCb(lv_timer_t* t);
    void Tick();

    lv_obj_t* root_ = nullptr;
    lv_obj_t* big_label_ = nullptr;              // 顶部大字(单一)
    lv_obj_t* sel_btn_[kPresets] = {};
    lv_obj_t* sel_lbl_[kPresets] = {};
    lv_obj_t* status_label_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    St st_ = St::SEL;
    int preset_s_[kPresets] = {30, 60, 120, 300, 480, 600};
    int sel_ = 3;
    int total_ = 300;
    int remain_ = 0;
    int done_ticks_ = 0;   // DONE 后计数(避让期后闪屏)
    bool active_ = false;
};
