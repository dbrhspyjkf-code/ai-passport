#pragma once

// lottery_app —— 双色球开奖查看器(边框卡片,红蓝分色)。
// 每页 3 期;每期一个圆角边框卡片(期号居中+红球红字+蓝球蓝字)。

#include "lvgl.h"

class LotteryApp {
public:
    static LotteryApp& Get();

    void Create(lv_obj_t* parent);
    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();
    void HandleKey(int key);

private:
    LotteryApp() = default;

    static const int kPerPage = 3;
    int page_ = 0;
    bool active_ = false;
    uint32_t seen_version_ = 0;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    // 每期: 卡片容器 + 期号行 + 红球行 + 蓝球+信息行
    lv_obj_t* cards_[kPerPage] = {};
    lv_obj_t* hdr_lbl_[kPerPage] = {};    // "125期 11-01"(居中)
    lv_obj_t* ball_lbl_[kPerPage] = {};   // 红球(红)+蓝球(蓝),用 recolor
    lv_obj_t* info_lbl_[kPerPage] = {};   // 销售奖池一等奖
    lv_obj_t* state_label_ = nullptr;
    lv_timer_t* sync_timer_ = nullptr;

    void Render();
    void SyncFromCache();
};
