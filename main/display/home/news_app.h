#pragma once

// news_app —— AI 新闻查看器(数据来自 NewsService,hermes /news/ai;
// 源头=OrangePi cron 每天中午生成的 AI 午报文件)。
// 交互:UP/DOWN=上一条/下一条(分区顺序拼接,末条=总结);OK=手动刷新;
// 长按方向键退出(统一 ExitCurrentApp)。
// 渲染直接读 NewsService 静态缓冲,不做 APP 侧拷贝(C3 内存红线,
// 数据只在 version 变化后重读)。

#include "lvgl.h"

class NewsApp {
public:
    static NewsApp& Get();

    // 创建 UI(持 LVGL 锁调用)。
    void Create(lv_obj_t* parent);

    bool Active() const { return active_; }

    void Enter();
    void Leave();
    void Destroy();

    // 按键路由(board 层持锁调用)。
    void HandleKey(int key);   // 0=上一条 1=下一条 2=刷新

private:
    NewsApp() = default;

    int cur_ = 0;                 // 当前条(含末尾总结位)
    uint32_t seen_version_ = 0;   // 上次渲染时的数据版本
    bool active_ = false;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* date_label_ = nullptr;    // 左上:日期头
    lv_obj_t* index_label_ = nullptr;   // 右上:n/N
    lv_obj_t* tag_label_ = nullptr;     // 分区名(必读/重要/其他/总结)
    lv_obj_t* body_label_ = nullptr;    // 条目正文(自动换行)
    lv_obj_t* hint_label_ = nullptr;    // 底部按键提示
    lv_timer_t* sync_timer_ = nullptr;

    int TotalEntries() const;     // 条目数 + 总结位
    void Render();
    void SyncFromCache();
};
