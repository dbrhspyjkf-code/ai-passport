#pragma once

// media_app —— 播放文件夹(主页第 4 格"播放"):原播放器(Sonos/HomePod)
// 与收音机的入口选择屏。上/下=切换选中,OK=进入,长按 OK=返回主页。
// 进入具体 APP 通过 HomeScreen::SwitchApp(3=播放器 8=收音机)。

#include <stdint.h>

#include "lvgl.h"

class MediaApp {
public:
    static MediaApp& Get();

    // 创建 UI(持 LVGL 锁调用)。
    void Create(lv_obj_t* parent);

    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();

    // 按键路由(board 层持锁调用)。
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK_CLICK

private:
    MediaApp() = default;

    static const int kEntryCount = 2;   // 0=音乐播放器 1=收音机

    lv_obj_t* root_ = nullptr;
    lv_obj_t* rows_[kEntryCount] = {};
    int sel_ = 0;
    bool active_ = false;

    void Highlight();
};
