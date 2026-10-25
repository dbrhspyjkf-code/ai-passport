#pragma once

// opi_app —— OrangePi 系统状态查看器(数据来自 OpiService,hermes /system/status,
// 与 Mac Übersicht 桌面 widget 同源)。
// UI: news_app 同构(头部 + 单 body 多行标签)+ 3 条进度条(负载/CPU/内存)。
// 进度条采用"删除重建"而非改尺寸:此 LVGL 配置下"创建后改宽度的矩形"
// 渲染不可靠(根因未定位),"创建时即最终尺寸"的矩形全屏验证可渲染
// (见 2026-10-25 条纹测试与固件记忆)。
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

    static const int kBarRows = 3;   // 负载/CPU/内存
    static const int kBarW = 224;    // 满条宽度(px)

    bool active_ = false;
    uint32_t seen_version_ = 0;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* host_label_ = nullptr;    // 页头:主机名 · uptime(左)
    lv_obj_t* ip_label_ = nullptr;      // 页头:IP(右)
    lv_obj_t* body_label_ = nullptr;    // 正文:负载/CPU/内存/温度 多行(news_app body 同款)
    lv_obj_t* bar_track_[kBarRows] = {};   // 轨道背景(Create 时一次成型,全宽暗条)
    lv_obj_t* bars_[kBarRows] = {};     // 进度条(渲染时删除重建,不动态改尺寸)
    lv_obj_t* state_label_ = nullptr;   // 错误/获取中提示
    lv_timer_t* sync_timer_ = nullptr;

    void RebuildBar(int row, float pct);
    void Render();
    void SyncFromCache();
};
