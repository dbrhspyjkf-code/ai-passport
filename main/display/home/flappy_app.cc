// flappy_app.cc —— Flappy Bird 实现(画面参考 FlapPyBird 原版配色)。
// 静态背景(天空/云/城市/灌木)建一次;管道/鸟/地面条纹常驻对象重定位。
#include "flappy_app.h"

LV_FONT_DECLARE(font_home_cn_16);
LV_FONT_DECLARE(font_pixel_clock_20);   // 点阵数字(局内分数)

#include "settings.h"
#include "application.h"
#include "assets/lang_config.h"

#include <cstdio>

#include "esp_random.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char* TAG = "flappy";

// ---- FlapPyBird 原版配色 ----
#define C_SKY 0x4ec0ca       // 天空青
#define C_CLOUD 0xf0fadc     // 云(米白)
#define C_CITY 0xd7e8c9      // 城市剪影(灰绿)
#define C_BUSH_L 0x7ac74f    // 灌木亮绿
#define C_BUSH_D 0x5fb844    // 灌木暗绿
#define C_BASE 0xded895      // 地面主体(米黄)
#define C_BASE_DK 0xcbb968   // 地面条纹暗黄
#define C_GRASS 0x73bf2e     // 地面顶草绿条 / 管道主绿
#define C_GRASS_L 0x9ce559   // 管体左高光
#define C_GRASS_D 0x589a25   // 管体右暗部
#define C_OUTLINE 0x543847   // 描边(原版深紫棕)
#define C_BIRD 0xfad02c      // 鸟体黄
#define C_WING 0xfdf0c8      // 翅奶油白
#define C_BEAK 0xf8672d      // 喙橙红
#define C_EYEBALL 0xffffff   // 眼白
#define C_PUPIL 0x2b2118     // 瞳孔

FlappyApp& FlappyApp::Get() {
    static FlappyApp instance;
    return instance;
}

// ---- OOM 保护(照 radio_app 教训:碎片堆上 lv_obj_create 会返回 NULL,
// 不判空=空指针 panic;更险的是字形渲染的 A8 缓冲分配失败会让
// LV_ASSERT_MALLOC 落进 while(1) → taskLVGL 卡死 → WDT 重启) ----
static bool s_ui_oom = false;

// 共享样式注册表:mk_rect 按 (色,圆角,描边) 组合建一个静态样式,对象
// 只挂样式引用。原来 82 个对象各带 5 条本地样式属性 ≈15KB,把堆吃光
// 正是"进 Flappy 卡死"的元凶;共享后 ≈6KB。
struct StyleKey { uint32_t color; int16_t radius; bool bordered; };
static StyleKey s_keys[28];
static lv_style_t s_styles[28];
static int s_style_n = 0;

static lv_style_t* get_style(uint32_t color, int radius, bool bordered) {
    for (int i = 0; i < s_style_n; i++) {
        if (s_keys[i].color == color && s_keys[i].radius == radius &&
            s_keys[i].bordered == bordered) {
            return &s_styles[i];
        }
    }
    if (s_style_n >= 28) return nullptr;
    int i = s_style_n++;
    s_keys[i] = {color, (int16_t)radius, bordered};
    lv_style_init(&s_styles[i]);
    lv_style_set_bg_color(&s_styles[i], lv_color_hex(color));
    lv_style_set_bg_opa(&s_styles[i], LV_OPA_COVER);
    lv_style_set_pad_all(&s_styles[i], 0);
    lv_style_set_radius(&s_styles[i], radius);
    if (bordered) {
        lv_style_set_border_color(&s_styles[i], lv_color_hex(C_OUTLINE));
        lv_style_set_border_width(&s_styles[i], 1);
    } else {
        lv_style_set_border_width(&s_styles[i], 0);
    }
    return &s_styles[i];
}

// 小矩形工具(共享样式;可选圆角/描边)
static lv_obj_t* mk_rect(lv_obj_t* parent, int w, int h, uint32_t color,
                         int radius = 0, bool bordered = false) {
    lv_obj_t* r = lv_obj_create(parent);
    if (r == nullptr) { s_ui_oom = true; return nullptr; }
    lv_style_t* st = get_style(color, radius, bordered);
    if (st == nullptr) { s_ui_oom = true; return nullptr; }
    lv_obj_remove_style_all(r);
    lv_obj_add_style(r, st, 0);
    lv_obj_set_size(r, w, h);
    return r;
}

void FlappyApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;
    s_ui_oom = false;
    // 任一阶段分配失败:删 root_(连带子对象)、清引用。用户看到的还是
    // 娱乐文件夹,堆回暖后再进会重试——"打不开"远好于"卡死重启"。
    auto fail = [this]() {
        ESP_LOGE(TAG, "UI create failed (OOM): free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        if (root_ != nullptr) lv_obj_delete(root_);
        ResetUiRefs();
    };

    root_ = lv_obj_create(parent);
    if (root_ == nullptr) { fail(); return; }
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(C_SKY), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 静态背景(云/城市/灌木/地面):先建=底层,管道从其前面经过 ----
    BuildScenery();
    if (s_ui_oom) { fail(); return; }

    // ---- 管道池(在背景之上,鸟/覆盖层之下) ----
    for (int i = 0; i < kPipeCount && !s_ui_oom; i++) {
        for (int s = 0; s < 2 && !s_ui_oom; s++) {
            lv_obj_t* ct = mk_rect(root_, kPipeW, 100, C_GRASS, 0, true);
            pipe_ct_[i][s] = ct;
            if (ct != nullptr) lv_obj_add_flag(ct, LV_OBJ_FLAG_HIDDEN);
            pipe_hl_[i][s] = mk_rect(ct, 4, 80, C_GRASS_L);
            if (pipe_hl_[i][s] != nullptr) lv_obj_set_pos(pipe_hl_[i][s], 4, 0);
            pipe_sh_[i][s] = mk_rect(ct, 4, 80, C_GRASS_D);
            if (pipe_sh_[i][s] != nullptr) lv_obj_set_pos(pipe_sh_[i][s], kPipeW - 9, 0);
            // 缘口宽帽(带描边+两道明暗条)
            lv_obj_t* lip = mk_rect(ct, 42, kLipH, C_GRASS, 0, true);
            pipe_lip_[i][s] = lip;
            lv_obj_t* lhl = mk_rect(lip, 4, kLipH - 4, C_GRASS_L);
            if (lhl != nullptr) lv_obj_set_pos(lhl, 5, 2);
            lv_obj_t* lsh = mk_rect(lip, 5, kLipH - 4, C_GRASS_D);
            if (lsh != nullptr) lv_obj_set_pos(lsh, 32, 2);
        }
    }
    if (s_ui_oom) { fail(); return; }
    // ---- 鸟:容器内拼 体/眼/瞳/喙/翅(视觉 17x12,判定 15x10) ----
    bird_ = lv_obj_create(root_);
    if (bird_ == nullptr) { s_ui_oom = true; }
    if (!s_ui_oom) {
        lv_obj_set_size(bird_, 17, 12);
        lv_obj_set_style_bg_opa(bird_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bird_, 0, 0);
        lv_obj_set_style_pad_all(bird_, 0, 0);
        lv_obj_set_style_radius(bird_, 0, 0);
        lv_obj_clear_flag(bird_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* body = mk_rect(bird_, 17, 12, C_BIRD, 4);
        if (body != nullptr) lv_obj_set_pos(body, 0, 0);
        lv_obj_t* eyeball = mk_rect(bird_, 6, 6, C_EYEBALL, 3);
        if (eyeball != nullptr) lv_obj_set_pos(eyeball, 8, 1);
        lv_obj_t* pupil = mk_rect(bird_, 2, 3, C_PUPIL);
        if (pupil != nullptr) lv_obj_set_pos(pupil, 11, 3);
        lv_obj_t* beak = mk_rect(bird_, 7, 4, C_BEAK, 1);
        if (beak != nullptr) lv_obj_set_pos(beak, 11, 7);
        wing_ = mk_rect(bird_, 8, 5, C_WING, 2);
        if (wing_ != nullptr) lv_obj_set_pos(wing_, 1, 3);
    }
    if (s_ui_oom) { fail(); return; }

    // ---- 局内分数:白字+深影(原版风格,顶部居中) ----
    score_shadow_ = lv_label_create(root_);
    score_label_ = lv_label_create(root_);
    overlay_label_ = lv_label_create(root_);
    if (score_shadow_ == nullptr || score_label_ == nullptr ||
        overlay_label_ == nullptr) {
        fail();
        return;
    }
    lv_obj_set_style_text_font(score_shadow_, &font_pixel_clock_20, 0);
    lv_obj_set_style_text_color(score_shadow_, lv_color_hex(C_OUTLINE), 0);
    lv_label_set_text(score_shadow_, "");
    lv_obj_align(score_shadow_, LV_ALIGN_TOP_MID, 2, 10);
    lv_obj_set_style_text_font(score_label_, &font_pixel_clock_20, 0);
    lv_obj_set_style_text_color(score_label_, lv_color_hex(0xffffff), 0);
    lv_label_set_text(score_label_, "");
    lv_obj_align(score_label_, LV_ALIGN_TOP_MID, 0, 8);

    // ---- 覆盖提示(READY/GAME OVER) ----
    lv_obj_set_style_text_font(overlay_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(overlay_label_, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_color(overlay_label_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay_label_, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(overlay_label_, 10, 0);
    lv_obj_set_style_radius(overlay_label_, 4, 0);
    lv_obj_set_style_text_align(overlay_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(overlay_label_, LV_ALIGN_CENTER, 0, -40);
    lv_label_set_text(overlay_label_, "");
    lv_obj_add_flag(overlay_label_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void FlappyApp::BuildScenery() {
    // 云带(米白圆顶,一排错落)
    static const int clouds[6][3] = {   // {x, y, w}
        {6, 148, 34}, {44, 154, 26}, {96, 144, 40}, {148, 152, 28},
        {186, 146, 36}, {224, 156, 22},
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t* c = mk_rect(root_, clouds[i][2], 12, C_CLOUD, 6);
        if (c == nullptr) return;
        lv_obj_set_pos(c, clouds[i][0], clouds[i][1]);
    }
    // 城市剪影(灰绿楼宇,底边统一 252)
    static const int city[9][3] = {   // {x, w, h}
        {0, 18, 30}, {20, 14, 20}, {38, 22, 36}, {64, 16, 24}, {84, 20, 32},
        {108, 14, 18}, {126, 24, 38}, {154, 16, 22}, {174, 22, 30},
    };
    for (int i = 0; i < 9; i++) {
        lv_obj_t* b = mk_rect(root_, city[i][1], city[i][2], C_CITY);
        if (b == nullptr) return;
        lv_obj_set_pos(b, city[i][0], 252 - city[i][2]);
    }
    lv_obj_t* b2 = mk_rect(root_, 26, 26, C_CITY);
    if (b2 == nullptr) return;
    lv_obj_set_pos(b2, 200, 226);
    lv_obj_t* b3 = mk_rect(root_, 14, 20, C_CITY);
    if (b3 == nullptr) return;
    lv_obj_set_pos(b3, 228, 232);
    // 灌木带(圆顶,亮暗交替,坐在地面带上)
    static const int bush[8][3] = {   // {x, w, h}
        {-4, 36, 16}, {30, 30, 12}, {58, 40, 18}, {96, 32, 13},
        {126, 38, 17}, {162, 30, 12}, {190, 40, 18}, {228, 30, 14},
    };
    for (int i = 0; i < 8; i++) {
        lv_obj_t* s = mk_rect(root_, bush[i][1], bush[i][2],
            (i % 2 == 0) ? C_BUSH_L : C_BUSH_D, bush[i][2] / 2);
        if (s == nullptr) return;
        lv_obj_set_pos(s, bush[i][0], kGroundTop - bush[i][2]);
    }
    // 地面:顶草绿条(带 1px 深线)+ 米黄主体 + 滚动条纹
    lv_obj_t* grass = mk_rect(root_, 240, 8, C_GRASS);
    if (grass == nullptr) return;
    lv_obj_set_pos(grass, 0, kGroundTop);
    lv_obj_t* gline = mk_rect(root_, 240, 2, C_BUSH_D);
    if (gline == nullptr) return;
    lv_obj_set_pos(gline, 0, kGroundTop + 8);
    lv_obj_t* base = mk_rect(root_, 240, 320 - kGroundTop - 10, C_BASE);
    if (base == nullptr) return;
    lv_obj_set_pos(base, 0, kGroundTop + 10);
    for (int i = 0; i < kGBands; i++) {
        gband_[i] = mk_rect(root_, 12, 320 - kGroundTop - 10, C_BASE_DK);
        if (gband_[i] == nullptr) return;
        lv_obj_set_pos(gband_[i], i * 30, kGroundTop + 10);
    }
}

void FlappyApp::ResetGame() {
    {   // NVS 读最高分
        Settings st("flappy", false);
        highscore_ = st.GetInt("highscore", 0);
    }
    st_ = St::READY;
    bird_y_ = 120;
    bird_vel_ = 0;
    score_ = 0;
    speed_ = 3;
    gap_ = 72;
    for (int i = 0; i < kPipeCount; i++) {
        pipe_x_[i] = -kPipeW - 2;   // READY 态管道全藏
        pipe_counted_[i] = false;
    }
    lv_label_set_text(score_label_, "");
    lv_label_set_text(score_shadow_, "");
    lv_label_set_text_fmt(overlay_label_, "FLAPPY\n最高 %d\n任意键开始", highscore_);
    lv_obj_clear_flag(overlay_label_, LV_OBJ_FLAG_HIDDEN);
    SyncObjects();
}

void FlappyApp::Flap() {
    if (st_ == St::READY) {
        st_ = St::PLAYING;
        lv_obj_add_flag(overlay_label_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(score_label_, "0");
        lv_label_set_text(score_shadow_, "0");
        // 管道等距入场:首根延迟 60px,后续各隔 kPipeSpacing
        for (int i = 0; i < kPipeCount; i++) {
            pipe_x_[i] = 240 + 60 + i * kPipeSpacing;
            pipe_gap_y_[i] = 40 + esp_random() % (kGroundTop - 80 - gap_);
            pipe_counted_[i] = false;
            RelayoutPipe(i);
        }
    }
    if (st_ == St::PLAYING) {
        bird_vel_ = kFlapV;
    }
}

void FlappyApp::SetOver() {
    st_ = St::OVER;
    // 破纪录:写 NVS
    if (score_ > highscore_) {
        highscore_ = score_;
        Settings st("flappy", true);
        st.SetInt("highscore", highscore_);
    }
    lv_label_set_text_fmt(overlay_label_,
        (score_ > 0 && score_ >= highscore_) ? "新纪录!\n%d 分\nOK 重新开始" : "GAME OVER\n%d 分\nOK 重新开始",
        score_);
    lv_obj_clear_flag(overlay_label_, LV_OBJ_FLAG_HIDDEN);
    // 结束音效:延迟 300ms 由 LVGL 一次性定时器触发,先渲完覆盖层
    // 中文字形再开声,避开"字形渲染(flash XIP)与 codec/I2S 重初始化
    // 并发 cache 停顿 → taskLVGL 卡死 → WDT 重启"(2026-10-23 现场)。
    lv_timer_create([](lv_timer_t* tm) {
        lv_timer_delete(tm);
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().GetAudioService().PlaySound(
                Lang::Sounds::OGG_CHIME_FULL);
        });
    }, 300, nullptr);
}

void FlappyApp::RelayoutPipe(int i) {
    // 上管:0..gap_y;下管:gap_y+gap..地面。缘口贴缝口。
    int h_top = pipe_gap_y_[i];
    lv_obj_set_size(pipe_ct_[i][0], kPipeW, h_top);
    lv_obj_set_size(pipe_hl_[i][0], 4, h_top > kLipH ? h_top - kLipH : 0);
    lv_obj_set_size(pipe_sh_[i][0], 4, h_top > kLipH ? h_top - kLipH : 0);
    lv_obj_set_pos(pipe_lip_[i][0], -4, h_top - kLipH);
    int h_bot = kGroundTop - pipe_gap_y_[i] - gap_;
    lv_obj_set_size(pipe_ct_[i][1], kPipeW, h_bot);
    int sb = h_bot > kLipH ? h_bot - kLipH : 0;
    lv_obj_set_size(pipe_hl_[i][1], 4, sb);
    lv_obj_set_size(pipe_sh_[i][1], 4, sb);
    lv_obj_set_pos(pipe_hl_[i][1], 4, kLipH);
    lv_obj_set_pos(pipe_sh_[i][1], kPipeW - 9, kLipH);
    lv_obj_set_pos(pipe_lip_[i][1], -4, 0);
}

void FlappyApp::Tick() {
    if (!active_) return;
    tick_no_++;

    if (st_ == St::PLAYING) {
        // 鸟:重力 + 限速(顶部只夹住不死)
        bird_vel_ += kGravity;
        if (bird_vel_ > kMaxFall) bird_vel_ = kMaxFall;
        bird_y_ += bird_vel_;
        if (bird_y_ < 0) { bird_y_ = 0; bird_vel_ = 0; }

        for (int i = 0; i < kPipeCount; i++) {
            pipe_x_[i] -= speed_;
            if (pipe_x_[i] < -kPipeW - 8) {   // 超出缘口宽即回收
                // 循环复用:跳到最右一根之后
                pipe_x_[i] += kPipeCount * kPipeSpacing;
                pipe_gap_y_[i] = 40 + esp_random() % (kGroundTop - 80 - gap_);
                pipe_counted_[i] = false;
                RelayoutPipe(i);
            }
            // 计分 + 难度(过缝 +1;每 8 分加速,每 10 分收缝)
            if (!pipe_counted_[i] && pipe_x_[i] + kPipeW < kBirdX) {
                pipe_counted_[i] = true;
                score_++;
                lv_label_set_text_fmt(score_label_, "%d", score_);
                lv_label_set_text_fmt(score_shadow_, "%d", score_);
                int sp = 3 + score_ / 8;
                speed_ = sp > 5 ? 5 : sp;
                int gp = 72 - 2 * (score_ / 10);
                gap_ = gp < 58 ? 58 : gp;
            }
            // 碰撞:鸟盒 vs 上/下管(含缘口外扩 4px)
            if (kBirdX < pipe_x_[i] + kPipeW + 4 && kBirdX + kBirdW > pipe_x_[i] - 4) {
                if (bird_y_ < pipe_gap_y_[i] || bird_y_ + kBirdH > pipe_gap_y_[i] + gap_) {
                    SetOver();
                }
            }
        }
        // 触地
        if (bird_y_ + kBirdH >= kGroundTop) SetOver();
    }

    SyncObjects();
}

void FlappyApp::SyncObjects() {
    if (root_ == nullptr) return;
    lv_obj_set_pos(bird_, kBirdX - 1, bird_y_ - 1);   // 视觉盒比判定盒大 1px
    // 翅膀扑动:每 3 tick 三拍循环(READY 态也在扇)
    static const int wy[3] = {1, 4, 6};
    lv_obj_set_pos(wing_, 1, wy[(tick_no_ / 3) % 3]);
    for (int i = 0; i < kPipeCount; i++) {
        bool show = (st_ != St::READY) && pipe_x_[i] > -kPipeW - 8 && pipe_x_[i] < 240;
        if (show) {
            lv_obj_clear_flag(pipe_ct_[i][0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(pipe_ct_[i][1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(pipe_ct_[i][0], pipe_x_[i], 0);
            lv_obj_set_pos(pipe_ct_[i][1], pipe_x_[i], pipe_gap_y_[i] + gap_);
        } else {
            lv_obj_add_flag(pipe_ct_[i][0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pipe_ct_[i][1], LV_OBJ_FLAG_HIDDEN);
        }
    }
    // 地面条纹随管道同速滚动(视差运动感)
    for (int i = 0; i < kGBands; i++) {
        int x = (i * 30 - (tick_no_ * speed_)) % 240;
        if (x < -12) x += 240 + 12;
        if (x > 240) x -= 240 + 12;
        lv_obj_set_pos(gband_[i], x, kGroundTop + 10);
    }
}

void FlappyApp::HandleKey(int key) {
    if (!active_ || root_ == nullptr) return;
    if (st_ == St::OVER) {
        if (key == 2) ResetGame();   // OK 重开(回 READY)
        return;
    }
    // READY/PLAYING:任意键拍翅
    Flap();
}

void FlappyApp::TickCb(lv_timer_t* t) {
    static_cast<FlappyApp*>(lv_timer_get_user_data(t))->Tick();
}

void FlappyApp::Enter() {
    if (root_ == nullptr) return;   // Create 因堆碎片失败:直接返回不进游戏
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && !active_) {
            lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
            active_ = true;
            ResetGame();
            if (tick_timer_ == nullptr) {
                tick_timer_ = lv_timer_create(TickCb, 30, this);
            } else {
                lv_timer_resume(tick_timer_);
            }
        }
        lvgl_port_unlock();
    }
}

void FlappyApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
        }
        ResetUiRefs();
        lvgl_port_unlock();
    }
}

void FlappyApp::ResetUiRefs() {
    root_ = nullptr;
    bird_ = nullptr;
    wing_ = nullptr;
    for (int i = 0; i < kPipeCount; i++) {
        for (int s = 0; s < 2; s++) {
            pipe_ct_[i][s] = nullptr;
            pipe_hl_[i][s] = nullptr;
            pipe_sh_[i][s] = nullptr;
            pipe_lip_[i][s] = nullptr;
        }
    }
    for (int i = 0; i < kGBands; i++) gband_[i] = nullptr;
    score_label_ = nullptr;
    score_shadow_ = nullptr;
    overlay_label_ = nullptr;
}

void FlappyApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && active_) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            active_ = false;
            if (tick_timer_ != nullptr) lv_timer_pause(tick_timer_);
        }
        lvgl_port_unlock();
    }
}
