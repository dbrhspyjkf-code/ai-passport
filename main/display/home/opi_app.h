#pragma once

// opi_app —— OrangePi 系统状态查看器(数据来自 OpiService,hermes /system/status,
// 与 Mac Übersicht 桌面 widget 同源)。
// UI 采用 news_app 同款极简结构(页头 + 单个多行正文标签)——多种自绘布局
// (分段条/轨道填充/单矩形条+独立数值标签)在此 LVGL 配置下仅部分行可见,
// 根因未明,先以已验证结构交付,条形图待根因定位后再加。
// 打开 APP 或 OK 才发起拉取(经 poller force 路径,无后台周期)。
// 交互:OK=刷新;UP/DOWN 无功能(消费);长按退出(统一 ExitCurrentApp)。

#include "lvgl.h"

class OpiApp {
public:
    static OpiApp& Get();

    // 创建 UI(持 LVGL 锁调用)。
    void Create(lv_obj_t* parent);

    bool Active() const { return active_; }

    void Enter();
    void Leave();
    void Destroy();

    // 按键路由(board 层持锁调用)。
    void HandleKey(int key);   // 2=刷新(0/1 无功能)

private:
    OpiApp() = default;

    bool active_ = false;
    uint32_t seen_version_ = 0;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* host_label_ = nullptr;    // 页头:主机名(左,亮) uptime(右,灰)
    lv_obj_t* body_label_ = nullptr;    // 正文:负载/CPU/内存/温度 多行(news_app body 同款)
    lv_obj_t* state_label_ = nullptr;   // 错误/获取中提示
    lv_timer_t* sync_timer_ = nullptr;

    void Render();
    void SyncFromCache();
};
