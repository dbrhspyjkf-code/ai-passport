#pragma once

// flappy_app —— Flappy Bird 小游戏(参考 FlapPyBird 复刻画面与手感)。
// 画面:天空+云+城市剪影+灌木(静态)+条纹地面(滚动)+带缘口管道+像素鸟。
// 手感:拍翅绑定按键按下瞬间(board 层 OnPressDown 即时触发),
// 30ms tick;管道横向滚动,穿缝计分;撞管/触地结束,OK 重开。
// 最高分 NVS("flappy")持久化;渲染用常驻对象重定位,不每帧重建。

#include <stdint.h>

#include "lvgl.h"

class FlappyApp {
public:
    static FlappyApp& Get();

    // 创建 UI(持 LVGL 锁调用)。
    void Create(lv_obj_t* parent);

    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();

    // 按键路由(board 层持锁调用;OnPressDown 即时进来,故响应灵敏)。
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK(游戏中均为拍翅)

private:
    FlappyApp() = default;

    // ---- 几何 ----
    static const int kBirdX = 54;       // 鸟固定横坐标(判定盒左缘)
    static const int kBirdW = 15;       // 判定盒(比视觉 17x12 略小,手感宽容)
    static const int kBirdH = 10;
    static const int kPipeW = 34;       // 管道体宽(缘口 42)
    static const int kLipH = 13;        // 缘口帽高
    static const int kGroundTop = 296;  // 地带上缘(游戏区 0..296)
    static const int kPipeCount = 3;    // 管道池(等距循环复用)
    static const int kPipeSpacing = 130;
    static const int kGBands = 8;       // 地面滚动条纹数

    // ---- 物理(px/tick, tick=30ms) ----
    static const int kFlapV = -7;
    static const int kGravity = 1;
    static const int kMaxFall = 8;

    // 游戏状态
    enum class St { READY, PLAYING, OVER };
    St st_ = St::READY;
    int bird_y_ = 120;
    int bird_vel_ = 0;
    int pipe_x_[kPipeCount] = {0, 0, 0};
    int pipe_gap_y_[kPipeCount] = {0, 0, 0};
    bool pipe_counted_[kPipeCount] = {false, false, false};
    int score_ = 0;
    int highscore_ = 0;   // 历史最高(NVS 持久化)
    int speed_ = 3;       // px/tick,随分数加快(3..5)
    int gap_ = 72;        // 缝高,随分数收窄(72..58)
    int tick_no_ = 0;     // 翅膀/地面动画节拍

    // UI
    lv_obj_t* root_ = nullptr;
    lv_obj_t* bird_ = nullptr;   // 鸟容器(体/眼/瞳/喙/翅矩形拼组,视觉 17x12)
    lv_obj_t* wing_ = nullptr;
    lv_obj_t* pipe_ct_[kPipeCount][2] = {};    // 管道容器 [i][0]=上 [i][1]=下
    lv_obj_t* pipe_hl_[kPipeCount][2] = {};    // 管体左高光条
    lv_obj_t* pipe_sh_[kPipeCount][2] = {};    // 管体右暗条
    lv_obj_t* pipe_lip_[kPipeCount][2] = {};   // 缘口宽帽
    lv_obj_t* gband_[kGBands] = {};            // 地面滚动条纹
    lv_obj_t* score_label_ = nullptr;
    lv_obj_t* score_shadow_ = nullptr;         // 分数投影(白字+深影,原版风)
    lv_obj_t* overlay_label_ = nullptr;        // READY/GAME OVER 提示
    lv_timer_t* tick_timer_ = nullptr;
    bool active_ = false;

    void BuildScenery();   // 天空/云/城市/灌木/地面(建一次)
    void ResetUiRefs();    // 清空全部 UI 对象引用(Destroy/Create 失败回退共用)
    void ResetGame();
    void Flap();
    void SetOver();
    void Tick();
    void SyncObjects();
    void RelayoutPipe(int i);   // 缝位置变化时重设管体尺寸
    static void TickCb(lv_timer_t* t);
};
