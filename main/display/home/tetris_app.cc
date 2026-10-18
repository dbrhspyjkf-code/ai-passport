// tetris_app.cc —— 俄罗斯方块实现。
// 渲染用矩形拼块(每格一个 10x10 rect 居中于 12px 格,留 1px 缝,像素感)。
#include "tetris_app.h"

LV_FONT_DECLARE(font_home_cn_16);

#include "settings.h"
#include "application.h"
#include "assets/lang_config.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "tetris";

// 复古荧光色板(色号 1-7)
static const uint32_t kColors[8] = {
    0,            // 0=空
    0xb7f5c4,     // 1 绿
    0xf5e663,     // 2 黄
    0x63d8f5,     // 3 青
    0xf56363,     // 4 红
    0xd563f5,     // 5 紫
    0xf5a263,     // 6 橙
    0xf5f5f5,     // 7 白
};

#define C_BG 0x0a1f14
#define C_FG 0xb7f5c4
#define C_DIM 0x4f7a5e
#define C_HINT 0x34573f

// 7 种方块的 4 格初始坐标(以 2x4/3x3 定义域)与 SRS 出生位置
struct PieceDef {
    int cells[4][2];   // {row, col} 相对坐标
};
static const PieceDef kPieces[7] = {
    {{{0, 0}, {0, 1}, {0, 2}, {0, 3}}},               // I
    {{{0, 0}, {0, 1}, {1, 0}, {1, 1}}},               // O
    {{{0, 1}, {1, 0}, {1, 1}, {1, 2}}},               // T
    {{{0, 0}, {1, 0}, {1, 1}, {1, 2}}},               // J
    {{{0, 2}, {1, 0}, {1, 1}, {1, 2}}},               // L
    {{{0, 1}, {1, 0}, {1, 1}, {2, 0}}},               // S
    {{{0, 0}, {1, 0}, {1, 1}, {2, 1}}},               // Z
};

TetrisApp& TetrisApp::Get() {
    static TetrisApp instance;
    return instance;
}

void TetrisApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 游戏区 120x240,居左留边
    board_canvas_ = lv_obj_create(root_);
    lv_obj_set_size(board_canvas_, kCols * kCell, kRows * kCell);
    lv_obj_set_style_bg_color(board_canvas_, lv_color_hex(0x081710), 0);
    lv_obj_set_style_border_color(board_canvas_, lv_color_hex(C_HINT), 0);
    lv_obj_set_style_border_width(board_canvas_, 1, 0);
    lv_obj_set_style_radius(board_canvas_, 0, 0);
    lv_obj_set_style_pad_all(board_canvas_, 0, 0);
    lv_obj_set_scrollbar_mode(board_canvas_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(board_canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(board_canvas_, LV_ALIGN_LEFT_MID, 12, 0);   // 左缘留 1 格

    // 很暗的格子线(静态层,建一次):竖 14 + 横 25
    lv_obj_t* grid = lv_obj_create(board_canvas_);
    lv_obj_set_size(grid, kCols * kCell, kRows * kCell);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(grid, 0, 0);
    for (int c2 = 0; c2 <= kCols; c2++) {
        lv_obj_t* ln = lv_obj_create(grid);
        lv_obj_set_size(ln, 1, kRows * kCell);
        lv_obj_set_style_bg_color(ln, lv_color_hex(0x0d1610), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ln, 0, 0);
        lv_obj_set_style_radius(ln, 0, 0);
        lv_obj_set_scrollbar_mode(ln, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(ln, c2 * kCell, 0);
    }
    for (int r2 = 0; r2 <= kRows; r2++) {
        lv_obj_t* ln = lv_obj_create(grid);
        lv_obj_set_size(ln, kCols * kCell, 1);
        lv_obj_set_style_bg_color(ln, lv_color_hex(0x0d1610), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ln, 0, 0);
        lv_obj_set_style_radius(ln, 0, 0);
        lv_obj_set_scrollbar_mode(ln, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(ln, 0, r2 * kCell);
    }
    // 动态格子层(每帧 clean 重建,不影响静态网格)
    cells_layer_ = lv_obj_create(board_canvas_);
    lv_obj_set_size(cells_layer_, kCols * kCell, kRows * kCell);
    lv_obj_set_style_bg_opa(cells_layer_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cells_layer_, 0, 0);
    lv_obj_set_style_pad_all(cells_layer_, 0, 0);
    lv_obj_set_scrollbar_mode(cells_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(cells_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(cells_layer_, 0, 0);

    // 右侧信息栏
    lv_obj_t* hud = lv_obj_create(root_);
    lv_obj_set_size(hud, 72, 236);   // 顶与游戏区齐,底到预览窗上方
    lv_obj_set_style_bg_opa(hud, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hud, 0, 0);
    lv_obj_set_style_pad_all(hud, 0, 0);
    lv_obj_set_flex_flow(hud, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hud, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(hud, 0, 0);
    lv_obj_set_scrollbar_mode(hud, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(hud, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(hud, LV_ALIGN_TOP_LEFT, 156, 16);   // 游戏区右缘起,顶与游戏区齐

    auto make_label = [&](void** slot) {
        lv_obj_t* l = lv_label_create(hud);
        lv_obj_set_style_text_color(l, lv_color_hex(C_FG), 0);
        lv_obj_set_style_text_font(l, &font_home_cn_16, 0);   // 16px 全字库(中文+数字)
        lv_obj_set_width(l, 72);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);   // 区域内居中
        *(lv_obj_t**)slot = l;
        return l;
    };
    make_label((void**)&high_label_);
    make_label((void**)&score_label_);
    make_label((void**)&lines_label_);
    make_label((void**)&level_label_);

    // 下一块预览 4x4(挂 root_,屏幕右下角,脱离 HUD flex)
    next_canvas_ = lv_obj_create(root_);
    lv_obj_set_size(next_canvas_, 52, 52);
    lv_obj_set_style_bg_color(next_canvas_, lv_color_hex(0x081710), 0);
    lv_obj_set_style_border_color(next_canvas_, lv_color_hex(C_HINT), 0);
    lv_obj_set_style_border_width(next_canvas_, 1, 0);
    lv_obj_set_style_radius(next_canvas_, 0, 0);
    lv_obj_set_style_pad_all(next_canvas_, 0, 0);
    lv_obj_set_scrollbar_mode(next_canvas_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(next_canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(next_canvas_, LV_ALIGN_BOTTOM_RIGHT, -12, -16);   // 右/底各留 1 格/对齐游戏区

    // 覆盖提示(暂停/结束):半透明黑底 + 白字,高对比
    overlay_label_ = lv_label_create(root_);
    lv_obj_set_style_text_color(overlay_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(overlay_label_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay_label_, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(overlay_label_, 10, 0);
    lv_obj_set_style_radius(overlay_label_, 4, 0);
    lv_obj_set_style_text_align(overlay_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(overlay_label_, kCols * kCell);
    lv_obj_align(overlay_label_, LV_ALIGN_LEFT_MID, 12, -20);
    lv_label_set_text(overlay_label_, "");
    lv_obj_add_flag(overlay_label_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void TetrisApp::ResetGame() {
    memset(board_, 0, sizeof(board_));
    {   // NVS 读最高分
        Settings st("tetris", false);
        highscore_ = st.GetInt("highscore", 0);
    }
    score_ = 0;
    lines_ = 0;
    level_ = 1;
    game_over_ = false;
    paused_ = false;
    tick_count_ = 0;
    next_type_ = esp_random() % 7;
    SpawnPiece();
    lv_obj_add_flag(overlay_label_, LV_OBJ_FLAG_HIDDEN);
    UpdateHud();
    DrawNext();
}

void TetrisApp::SpawnPiece() {
    cur_type_ = next_type_;
    next_type_ = esp_random() % 7;
    const PieceDef& p = kPieces[cur_type_];
    for (int i = 0; i < 4; i++) {
        cur_[i][0] = p.cells[i][0];
        cur_[i][1] = p.cells[i][1] + 3;   // 出生列 3-6 居中
    }
    if (Collides(cur_, 0, 0)) {
        game_over_ = true;
        // 破纪录:写 NVS
        if (score_ > highscore_) {
            highscore_ = score_;
            Settings st("tetris", true);
            st.SetInt("highscore", highscore_);
        }
        lv_label_set_text_fmt(overlay_label_,
            (score_ > 0 && score_ >= highscore_) ? "新纪录!\n%d 分\nOK 重新开始" : "GAME OVER\n%d 分\nOK 重新开始",
            score_);
        lv_obj_clear_flag(overlay_label_, LV_OBJ_FLAG_HIDDEN);
        // 结束音效:独立任务推官方解码队列(与计时提示音同模式)
        static bool s_snd_busy = false;
        if (!s_snd_busy) {
            s_snd_busy = true;
            xTaskCreate(
                [](void*) {
                    Application::GetInstance().GetAudioService().PlaySound(
                        Lang::Sounds::OGG_EXCLAMATION);
                    vTaskDelay(pdMS_TO_TICKS(1500));   // 音效 ~1s,等排空
                    s_snd_busy = false;
                    vTaskDelete(nullptr);
                },
                "tetris_snd", 8192, nullptr, 3, nullptr);
        }
    }
    DrawNext();
}

bool TetrisApp::Collides(const int cells[4][2], int row_off, int col_off) const {
    for (int i = 0; i < 4; i++) {
        int r = cells[i][0] + row_off;
        int c = cells[i][1] + col_off;
        if (r < 0 || r >= kRows || c < 0 || c >= kCols) return true;
        if (r >= 0 && board_[r][c] != 0) return true;
    }
    return false;
}

void TetrisApp::LockPiece() {
    for (int i = 0; i < 4; i++) {
        board_[cur_[i][0]][cur_[i][1]] = cur_type_ + 1;
    }
    int cleared = ClearLines();
    if (cleared > 0) {
        static const int kScores[5] = {0, 100, 300, 500, 800};
        score_ += kScores[cleared] * level_;
        lines_ += cleared;
        level_ = 1 + lines_ / 10;
        UpdateHud();
    }
    SpawnPiece();
}

int TetrisApp::ClearLines() {
    int cleared = 0;
    for (int r = kRows - 1; r >= 0; r--) {
        bool full = true;
        for (int c = 0; c < kCols; c++) {
            if (board_[r][c] == 0) { full = false; break; }
        }
        if (full) {
            cleared++;
            for (int rr = r; rr > 0; rr--) {
                memcpy(board_[rr], board_[rr - 1], sizeof(board_[rr]));
            }
            memset(board_[0], 0, sizeof(board_[0]));
            r++;   // 同一行再查(已下移)
        }
    }
    return cleared;
}

void TetrisApp::RotatePiece() {
    if (game_over_ || paused_) return;
    int rotated[4][2];
    // 绕第 0 格旋转: (r,c) -> (c, -r)
    for (int i = 0; i < 4; i++) {
        rotated[i][0] = cur_[i][1];
        rotated[i][1] = -cur_[i][0];
    }
    // 平移回非负区域,再试碰撞(含踢墙:0/±1/±2 列)
    int min_c = 99, min_r = 99;
    for (int i = 0; i < 4; i++) {
        if (rotated[i][1] < min_c) min_c = rotated[i][1];
        if (rotated[i][0] < min_r) min_r = rotated[i][0];
    }
    for (int i = 0; i < 4; i++) {
        rotated[i][0] -= min_r;
        rotated[i][1] -= min_c;
    }
    // 对齐回原锚点(以原第 0 格为基准)
    int base_r = cur_[0][0], base_c = cur_[0][1];
    for (int kick = 0; kick <= 2; kick++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            int off = kick * sign;
            int cand[4][2];
            for (int i = 0; i < 4; i++) {
                cand[i][0] = rotated[i][0] + base_r;
                cand[i][1] = rotated[i][1] + base_c + off;
            }
            if (!Collides(cand, 0, 0)) {
                memcpy(cur_, cand, sizeof(cur_));
                DrawBoard();
                return;
            }
            if (kick == 0) break;   // 0 只试一次
        }
    }
}

void TetrisApp::HardDrop() {
    if (game_over_ || paused_) return;
    int off = 0;
    while (!Collides(cur_, off + 1, 0)) off++;
    for (int i = 0; i < 4; i++) cur_[i][0] += off;
    LockPiece();
    DrawBoard();
}

void TetrisApp::StepDown() {
    if (game_over_ || paused_) return;
    if (!Collides(cur_, 1, 0)) {
        for (int i = 0; i < 4; i++) cur_[i][0]++;
    } else {
        LockPiece();
    }
    DrawBoard();
}

void TetrisApp::HandleKey(int key) {
    if (!active_ || root_ == nullptr) return;
    if (game_over_) {
        if (key == 2) ResetGame();   // OK 重开
        return;
    }
    if (key == 5) {   // 长按下:快速落下方块(硬降)
        HardDrop();
        return;
    }
    if (paused_) return;
    switch (key) {
        case 0:   // 上=左移
            if (!Collides(cur_, 0, -1)) {
                for (int i = 0; i < 4; i++) cur_[i][1]--;
                DrawBoard();
            }
            break;
        case 1:   // 下=右移
            if (!Collides(cur_, 0, 1)) {
                for (int i = 0; i < 4; i++) cur_[i][1]++;
                DrawBoard();
            }
            break;
        case 2:   // OK=旋转
            RotatePiece();
            break;
        default:
            break;
    }
}

void TetrisApp::DrawBoard() {
    if (cells_layer_ == nullptr) return;
    lv_obj_clean(cells_layer_);
    // 已锁定格
    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < kCols; c++) {
            uint8_t v = board_[r][c];
            if (v == 0) continue;
            lv_obj_t* cell = lv_obj_create(cells_layer_);
            lv_obj_set_size(cell, kCell - 2, kCell - 2);
            lv_obj_set_style_bg_color(cell, lv_color_hex(kColors[v]), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_radius(cell, 0, 0);
            lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(cell, c * kCell + 1, r * kCell + 1);
        }
    }
    // 当前方块
    if (!game_over_) {
        for (int i = 0; i < 4; i++) {
            int r = cur_[i][0], c = cur_[i][1];
            if (r < 0) continue;
            lv_obj_t* cell = lv_obj_create(cells_layer_);
            lv_obj_set_size(cell, kCell - 2, kCell - 2);
            lv_obj_set_style_bg_color(cell, lv_color_hex(kColors[cur_type_ + 1]), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_radius(cell, 0, 0);
            lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(cell, c * kCell + 1, r * kCell + 1);
        }
    }
}

void TetrisApp::DrawNext() {
    if (next_canvas_ == nullptr) return;
    lv_obj_clean(next_canvas_);
    const PieceDef& p = kPieces[next_type_];
    for (int i = 0; i < 4; i++) {
        lv_obj_t* cell = lv_obj_create(next_canvas_);
        lv_obj_set_size(cell, kCell - 2, kCell - 2);
        lv_obj_set_style_bg_color(cell, lv_color_hex(kColors[next_type_ + 1]), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_radius(cell, 0, 0);
        lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        // 预览居中于 52x52
        lv_obj_set_pos(cell, 14 + p.cells[i][1] * kCell, 6 + p.cells[i][0] * kCell);
    }
}

void TetrisApp::UpdateHud() {
    if (score_label_ == nullptr) return;
    lv_label_set_text_fmt(high_label_, "最高\n%d", highscore_);
    lv_label_set_text_fmt(score_label_, "分数\n%d", score_);
    lv_label_set_text_fmt(lines_label_, "行数\n%d", lines_);
    lv_label_set_text_fmt(level_label_, "等级\n%d", level_);
}

void TetrisApp::TickCb(lv_timer_t* t) {
    static_cast<TetrisApp*>(lv_timer_get_user_data(t))->Tick();
}

void TetrisApp::Tick() {
    if (!active_) return;
    tick_count_++;
    // 等级越高下落越快:等级 n 每 (11-n) 个 tick(100ms) 降一行
    int interval = 11 - level_;
    if (interval < 2) interval = 2;
    if (tick_count_ % (uint32_t)interval == 0) {
        StepDown();
    }
}

void TetrisApp::Enter() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && !active_) {
            lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
            active_ = true;
            ResetGame();
            DrawBoard();
            if (tick_timer_ == nullptr) {
                tick_timer_ = lv_timer_create(TickCb, 100, this);
            } else {
                lv_timer_resume(tick_timer_);
            }
        }
        lvgl_port_unlock();
    }
}

void TetrisApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        lvgl_port_unlock();
    }
}

void TetrisApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && active_) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            active_ = false;
            if (tick_timer_ != nullptr) lv_timer_pause(tick_timer_);
        }
        lvgl_port_unlock();
    }
}
