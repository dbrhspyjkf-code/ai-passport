#pragma once

// answer_app —— 答案之书(参考 M5-DIAL AnswerBook,数据同源 ~120 条)。
// OK 短按:翻开一条答案(快速轮转+减速定格,避开上一条);
// OK 长按:退出。夜空像素风:深蓝紫底+星点+青色标题。

#include "lvgl.h"

class AnswerApp {
public:
    static AnswerApp& Get();

    void Create(lv_obj_t* parent);
    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK_CLICK 3=OK_LONG

private:
    AnswerApp() = default;

    void ShowPrompt();
    void StartFlip();
    void FinishFlip();
    static void FlipTickCb(lv_timer_t* t);
    void FlipTick();

    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* answer_label_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_timer_t* flip_timer_ = nullptr;
    bool active_ = false;
    bool flipping_ = false;
    int flip_step_ = 0;
    int last_answer_ = -1;
};
