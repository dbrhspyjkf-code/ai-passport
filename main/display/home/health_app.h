#pragma once

// health_app —— 健康数据查看器(数据来自 HealthService,hermes /api/health,
// iPhone 快捷指令推送;与 M5Tab5 app_health 同源)。
// UI: 2 列×5 行彩色边框卡片(名称+数值),每卡固定尺寸(改宽度不渲染坑);
// 页头显示数据更新时刻。打开/OK 才拉取(poller force 路径)。
// 交互:OK=刷新;UP/DOWN 无功能(消费);长按退出(统一 ExitCurrentApp)。

#include "lvgl.h"

class HealthApp {
public:
    static HealthApp& Get();

    void Create(lv_obj_t* parent);

    bool Active() const { return active_; }

    void Enter();
    void Leave();
    void Destroy();

    void HandleKey(int key);   // 2=刷新(0/1 无功能)

private:
    HealthApp() = default;

    static const int kRows = 5;
    static const int kCols = 2;
    static const int kCardW = 108;   // (240-16-8)/2
    static const int kCardH = 46;

    bool active_ = false;
    uint32_t seen_version_ = 0;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* updated_label_ = nullptr;
    lv_obj_t* cards_[kRows][kCols] = {};      // 卡片容器
    lv_obj_t* name_lbl_[kRows][kCols] = {};   // 指标名(dim)
    lv_obj_t* val_lbl_[kRows][kCols] = {};    // 数值+单位(亮)
    lv_obj_t* state_label_ = nullptr;
    lv_timer_t* sync_timer_ = nullptr;

    void Render();
    void SyncFromCache();
};