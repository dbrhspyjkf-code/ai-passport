#pragma once

// games_app —— 娱乐文件夹(主页第 6 格"娱乐"):列出小游戏供选择。
// 上/下=切换选中,OK=进入游戏,长按 OK=返回主页(board 层 Leave)。
// 进入具体游戏通过 HomeScreen::SwitchApp(6=方块 7=Flappy)。
// 收音机已移至"播放"文件夹(media_app,2026-10-25)。

#include <stdint.h>

#include "lvgl.h"

class GamesApp {
public:
    static GamesApp& Get();

    // 创建 UI(持 LVGL 锁调用)。
    void Create(lv_obj_t* parent);

    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();

    // 按键路由(board 层持锁调用)。
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK_CLICK

private:
    GamesApp() = default;

    static const int kGameCount = 2;   // 0=俄罗斯方块 1=Flappy Bird

    lv_obj_t* root_ = nullptr;
    lv_obj_t* rows_[kGameCount] = {};
    lv_obj_t* score_labels_[kGameCount] = {};   // 行内"最高 N"
    int sel_ = 0;
    bool active_ = false;

    void RefreshScores();   // 每次进入重读 NVS 最高分
    void Highlight();
};
