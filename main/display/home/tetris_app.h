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
    lv_obj_t* cells_layer_ = nullptr;   // 动态格子层(池化,见 cell_pool_)
    lv_obj_t* board_canvas_ = nullptr;    // 网格绘制区
    lv_obj_t* high_label_ = nullptr;   // 最高分
    lv_obj_t* score_label_ = nullptr;
    lv_obj_t* lines_label_ = nullptr;
    lv_obj_t* level_label_ = nullptr;
    lv_obj_t* next_canvas_ = nullptr;     // 下一块预览
    lv_obj_t* next_pool_[4] = {};         // 预览格池(4 格,免建删)
    lv_obj_t* overlay_label_ = nullptr;   // 暂停/结束提示
    lv_timer_t* tick_timer_ = nullptr;
    bool active_ = false;
    uint32_t tick_count_ = 0;

    // 绘制分两层(2026-10-25 重构,兼顾"改宽度不渲染"坑与内存红线):
    //  - 锁定格 = DrawBoardSegments() 行程编码段,仅在棋盘变化(落定/消行/
    //    重开,≤1次/秒)时 clean+重建,段对象"出生即最终宽度";满盘典型
    //    40-80 段 ≈3-8KB(一格一对象旧画法满盘 ~280 对象 ≈24KB,曾是
    //    DNS 掉线/bad_alloc 重启总根因)。
    //  - 当前方块 = DrawBoard() 4 个固定尺寸池化格,每帧只 set_pos。
    static const int kPoolSize = 4;   // 仅当前方块 4 格
    lv_obj_t* cell_pool_[kPoolSize] = {};
    lv_obj_t* PoolGet(int idx);

    void ResetGame();
    void SpawnPiece();
    bool Collides(const int cells[4][2], int row_off, int col_off) const;
    void LockPiece();
    int ClearLines();
    void DrawBoardSegments();
    void RotatePiece();
    void HardDrop();
    void StepDown();
    void DrawBoard();
    void DrawNext();
    void UpdateHud();
    static void TickCb(lv_timer_t* t);
    void Tick();
};
