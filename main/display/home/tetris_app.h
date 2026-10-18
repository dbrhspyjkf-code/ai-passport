#pragma once

// tetris_app —— 俄罗斯方块(三键:上=左移,下=右移,OK=旋转,OK长按=硬降)。
// 重力自动下落(速度随等级加快);消行计分;游戏结束 OK 重开。
// 渲染:10x20 网格 12px 格 + 右侧信息栏(分数/行数/下一块预览)。

#include <stdint.h>

#include "lvgl.h"

class TetrisApp {
public:
    static TetrisApp& Get();

    // 创建 UI(持 LVGL 锁调用)。
    void Create(lv_obj_t* parent);

    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();

    // 按键路由(board 层持锁调用)。
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK_CLICK 3=OK_LONG

private:
    TetrisApp() = default;

    static const int kCols = 12;   // 144px 宽(左右各留 12px)
    static const int kRows = 24;   // 288px 高
    static const int kCell = 12;   // 像素/格(12x24 格 = 144x288)

    // 游戏状态
    uint8_t board_[kRows][kCols];   // 0 空,1-7 方块色号
    int cur_[4][2];                 // 当前方块 4 格坐标 {row,col}
    int cur_type_ = 0;
    int next_type_ = 0;
    int score_ = 0;
    int highscore_ = 0;   // 历史最高(NVS 持久化)
    int lines_ = 0;
    int level_ = 1;
    bool game_over_ = false;
    bool paused_ = false;

    // UI
    lv_obj_t* root_ = nullptr;
    lv_obj_t* cells_layer_ = nullptr;   // 动态格子层(每帧重建)
    lv_obj_t* board_canvas_ = nullptr;    // 网格绘制区
    lv_obj_t* high_label_ = nullptr;   // 最高分
    lv_obj_t* score_label_ = nullptr;
    lv_obj_t* lines_label_ = nullptr;
    lv_obj_t* level_label_ = nullptr;
    lv_obj_t* next_canvas_ = nullptr;     // 下一块预览
    lv_obj_t* overlay_label_ = nullptr;   // 暂停/结束提示
    lv_timer_t* tick_timer_ = nullptr;
    bool active_ = false;
    uint32_t tick_count_ = 0;

    void ResetGame();
    void SpawnPiece();
    bool Collides(const int cells[4][2], int row_off, int col_off) const;
    void LockPiece();
    int ClearLines();
    void RotatePiece();
    void HardDrop();
    void StepDown();
    void DrawBoard();
    void DrawNext();
    void UpdateHud();
    static void TickCb(lv_timer_t* t);
    void Tick();
};
