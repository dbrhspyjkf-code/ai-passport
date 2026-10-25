// home_screen.cc —— 像素名片主页实现。
//
// 配色:深墨绿底 + 荧光绿前景,复古单色掌机风(与 1-bit 素材一致)。
// 字体:名字用小智内置中文字体;次级文本用 16px 子集字体;点阵时钟独立字体。
//
// 个人信息(名字/签名/城市/坐标)在本文件顶部常量区,改完重编译即可。
#include "home_screen.h"

#include "material_symbols.h"

#include <cinttypes>
#include <cstdio>
#include <ctime>

#include "display.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "agenda_service.h"
#include "glm_service.h"
#include "deepseek_service.h"
#include "lunar_calendar.h"
#include "weather_service.h"
#include "mail_app.h"
#include "news_app.h"
#include "tools_app.h"
#include "opi_app.h"
#include "finance_app.h"
#include "tetris_app.h"
#include "flappy_app.h"
#include "radio_app.h"
#include "media_app.h"
#include "games_app.h"
#include "answer_app.h"
#include "player_app.h"
#include "timer_app.h"

// ---------------- 个人信息配置 ----------------
#define HOME_NAME "阿皮"
#define HOME_SIGNATURE "慢就是快"
#define HOME_CITY "罗湖"
#define HOME_TITLE "PASSPORT"

// ---------------- 配色 ----------------
#define C_BG 0x0a1f14      // 深墨绿底
#define C_FG 0xb7f5c4      // 荧光绿主前景
#define C_DIM 0x4f7a5e     // 次级暗绿
#define C_HINT 0x34573f
#define C_TITLE_X 0x8ae0f7   // 答案之书图标青    // 更暗(按键提示/框线)

// 点阵时钟数字字体(Press Start 2P, OFL;lv_font_conv 生成,24px)
LV_FONT_DECLARE(font_pixel_clock_20);
// 主页次级文本小字体(思源黑体 16px 子集,lv_font_conv 生成)
LV_FONT_DECLARE(font_home_small_16);
LV_FONT_DECLARE(font_home_cn_16);
LV_FONT_DECLARE(font_material_symbols_20_4);

LV_IMAGE_DECLARE(img_home_avatar);
LV_IMAGE_DECLARE(img_home_avatar_blink);

HomeScreen& HomeScreen::Get() {
    static HomeScreen instance;
    return instance;
}

void HomeScreen::Create(lv_obj_t* parent, const lv_font_t* font) {
    home_font_ = font;
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 顶部状态栏:WIFI | PASSPORT ····· 电量% + 电池 ----
    lv_obj_t* top = lv_obj_create(root_);
    lv_obj_set_size(top, LV_HOR_RES, 26);
    lv_obj_set_style_bg_color(top, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(top, lv_color_hex(C_HINT), 0);
    lv_obj_set_style_border_width(top, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_pad_left(top, 8, 0);
    lv_obj_set_style_pad_right(top, 8, 0);
    lv_obj_set_scrollbar_mode(top, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    // WIFI 图标(与小智页面同款 Material Symbols)
    lv_obj_t* wifi_icon = lv_label_create(top);
    lv_obj_set_style_text_font(wifi_icon, &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(C_FG), 0);
    lv_label_set_text(wifi_icon, MATERIAL_SYMBOLS_WIFI);
    lv_obj_align(wifi_icon, LV_ALIGN_LEFT_MID, 0, 0);

    // 像素电池:外框 + 电量填充 + 百分比文字
    // 未读邮件提示(顶部中央):闪烁邮件图标(与小智同款 Material Symbols)
    mail_label_ = lv_label_create(top);
    lv_obj_set_style_text_font(mail_label_, &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(mail_label_, lv_color_hex(0xFFC24B), 0);
    lv_label_set_text(mail_label_, MATERIAL_SYMBOLS_MAIL);
    lv_obj_align(mail_label_, LV_ALIGN_CENTER, 0, 0);
    mail_blink_ = lv_timer_create([](lv_timer_t* t) {
        auto* self = static_cast<HomeScreen*>(lv_timer_get_user_data(t));
        // 显/隐交替(1 秒周期)
        if (lv_obj_has_flag(self->mail_label_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(self->mail_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(self->mail_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }, 1000, this);
    lv_timer_pause(mail_blink_);
    lv_obj_add_flag(mail_label_, LV_OBJ_FLAG_HIDDEN);

    battery_text_ = lv_label_create(top);
    lv_obj_set_style_text_font(battery_text_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(battery_text_, lv_color_hex(C_FG), 0);
    lv_label_set_text(battery_text_, "--%");
    lv_obj_align(battery_text_, LV_ALIGN_RIGHT_MID, -28, 0);

    battery_frame_ = lv_obj_create(top);
    lv_obj_set_size(battery_frame_, 24, 12);
    lv_obj_set_style_bg_color(battery_frame_, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(battery_frame_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(battery_frame_, lv_color_hex(C_FG), 0);
    lv_obj_set_style_border_width(battery_frame_, 1, 0);
    lv_obj_set_style_radius(battery_frame_, 0, 0);
    lv_obj_set_style_pad_all(battery_frame_, 1, 0);
    lv_obj_set_scrollbar_mode(battery_frame_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(battery_frame_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(battery_frame_, LV_ALIGN_RIGHT_MID, -3, 0);

    lv_obj_t* nub = lv_obj_create(top);
    lv_obj_set_size(nub, 2, 6);
    lv_obj_set_style_bg_color(nub, lv_color_hex(C_FG), 0);
    lv_obj_set_style_border_width(nub, 0, 0);
    lv_obj_set_style_radius(nub, 0, 0);
    lv_obj_set_scrollbar_mode(nub, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(nub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(nub, LV_ALIGN_RIGHT_MID, 0, 0);

    battery_fill_ = lv_obj_create(battery_frame_);
    lv_obj_set_height(battery_fill_, LV_PCT(100));
    lv_obj_set_width(battery_fill_, LV_PCT(100));
    lv_obj_set_style_bg_color(battery_fill_, lv_color_hex(C_FG), 0);
    lv_obj_set_style_border_width(battery_fill_, 0, 0);
    lv_obj_set_style_radius(battery_fill_, 0, 0);
    lv_obj_set_scrollbar_mode(battery_fill_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(battery_fill_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(battery_fill_, LV_ALIGN_LEFT_MID, 0, 0);

    // ---- 左栏:像素头像 + GLM 用量(文本 + 进度条) ----
    lv_obj_t* avatar_box = lv_obj_create(root_);
    lv_obj_set_size(avatar_box, 100, 100);
    lv_obj_set_style_bg_color(avatar_box, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_color(avatar_box, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_border_width(avatar_box, 1, 0);
    lv_obj_set_style_pad_all(avatar_box, 1, 0);
    lv_obj_set_style_radius(avatar_box, 16, 0);   // 圆角头像框
    lv_obj_set_scrollbar_mode(avatar_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(avatar_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_box, LV_ALIGN_TOP_LEFT, 6, 34);

    avatar_img_ = lv_image_create(avatar_box);
    lv_image_set_src(avatar_img_, &img_home_avatar);
    lv_obj_set_style_radius(avatar_img_, 15, 0);   // 图片四角同步裁圆(clip_corner)
    lv_obj_center(avatar_img_);

    // 眨眼:闭眼图片帧切换(每 10 秒闭合 150ms)
    blink_timer_ = lv_timer_create([](lv_timer_t* t) {
        auto* self = static_cast<HomeScreen*>(lv_timer_get_user_data(t));
        self->blink_closed_ = !self->blink_closed_;
        lv_image_set_src(self->avatar_img_,
                         self->blink_closed_ ? &img_home_avatar_blink : &img_home_avatar);
        lv_timer_set_period(t, self->blink_closed_ ? 150 : 10000);
    }, 10000, this);

    glm_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(glm_label_, &font_home_small_16, 0);
    lv_obj_set_style_text_color(glm_label_, lv_color_hex(C_FG), 0);
    lv_label_set_text(glm_label_, "GLM …");
    lv_obj_align(glm_label_, LV_ALIGN_TOP_LEFT, 8, 146);

    // ---- GLM 右侧:DeepSeek 余额(与右栏日期/天气同轴;y 同 GLM 文本行) ----
    // 动态文本:用全量中文字体(font_home_small_16 子集无 D/S/元 字形)
    ds_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(ds_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(ds_label_, lv_color_hex(C_FG), 0);
    lv_label_set_text(ds_label_, "DS …");
    lv_obj_align(ds_label_, LV_ALIGN_TOP_MID, 53, 146);

    // ---- GLM 下方:最近的提醒/行程(hermes /agenda) ----
    // 动态中文文本:全量中文字体(1bpp 16px GB2312 子集不会缺字)
    agenda_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(agenda_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(agenda_label_, lv_color_hex(C_FG), 0);
    lv_label_set_text(agenda_label_, "");
    lv_obj_set_width(agenda_label_, LV_HOR_RES - 16);
    lv_label_set_long_mode(agenda_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(agenda_label_, LV_ALIGN_TOP_LEFT, 8, 186);

    // ---- 右栏:点阵时钟 + 日期 + 农历 + 位置 + 天气 ----
    clock_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(clock_label_, &font_pixel_clock_20, 0);
    lv_obj_set_style_text_color(clock_label_, lv_color_hex(C_FG), 0);
    lv_label_set_text(clock_label_, "--:--");
    lv_obj_align(clock_label_, LV_ALIGN_TOP_MID, 53, 36);   // 头像框右缘~屏幕右缘居中

    date_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(date_label_, &font_home_small_16, 0);
    lv_obj_set_style_text_color(date_label_, lv_color_hex(C_FG), 0);
    lv_label_set_text(date_label_, "");
    lv_obj_align(date_label_, LV_ALIGN_TOP_MID, 53, 66);

    lunar_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(lunar_label_, &font_home_small_16, 0);
    lv_obj_set_style_text_color(lunar_label_, lv_color_hex(C_FG), 0);
    lv_label_set_text(lunar_label_, "");
    lv_obj_align(lunar_label_, LV_ALIGN_TOP_MID, 53, 88);

    weather_line1_ = lv_label_create(root_);
    lv_obj_set_style_text_font(weather_line1_, &font_home_small_16, 0);
    lv_obj_set_style_text_color(weather_line1_, lv_color_hex(C_FG), 0);
    lv_label_set_text(weather_line1_, "");
    lv_obj_align(weather_line1_, LV_ALIGN_TOP_MID, 53, 110);

    // ---- 底部:APP 启动栏(图标在横线上方,名字+提示在横线下方) ----
    lv_obj_t* divider = lv_obj_create(root_);
    lv_obj_set_size(divider, LV_HOR_RES, 1);   // 全宽,与顶栏横线一致
    lv_obj_set_style_bg_color(divider, lv_color_hex(C_HINT), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_scrollbar_mode(divider, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, -31);   // 文字在横线与屏底间垂直居中

    // 槽位:0 金融 1 邮件 2 播放文件夹 3 计时 4 娱乐文件夹 5 AI 新闻
    // (答案之书 2026-10-25 移入娱乐文件夹,不再占启动栏)
    for (int i = 0; i < kLauncherCount; i++) {
        launcher_items_[i] = lv_obj_create(root_);
    }
    for (int i = 0; i < kLauncherCount; i++) {
        lv_obj_set_size(launcher_items_[i], 32, 32);
        lv_obj_set_style_bg_color(launcher_items_[i], lv_color_hex(C_BG), 0);
        lv_obj_set_style_border_color(launcher_items_[i], lv_color_hex(C_DIM), 0);
        lv_obj_set_style_border_width(launcher_items_[i], 1, 0);
        lv_obj_set_style_radius(launcher_items_[i], 6, 0);   // 圆角图标框
        lv_obj_set_style_pad_all(launcher_items_[i], 0, 0);
        lv_obj_set_scrollbar_mode(launcher_items_[i], LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(launcher_items_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(launcher_items_[i], 4 + i * 40, 248);   // 6 格 32px,40px 间距居中
    }
    {   // 金融:K 线蜡烛图(6 根,左下→右上上升,红绿混合)
        auto px = [&](int x, int y, int w, int h, uint32_t color) {
            lv_obj_t* b = lv_obj_create(launcher_items_[0]);
            lv_obj_set_size(b, w, h);
            lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_radius(b, 0, 0);
            lv_obj_set_scrollbar_mode(b, LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(b, x, y);
        };
        const uint32_t RED = 0xF5172A, GREEN = 0x12A844;
        // 每根:影线(1px 竖) + 实体(3px 横)。走势:左下(高19)→右上(高3)
        // {实体x, 实体y, 影线顶y, 影线高, 颜色}
        const int kCandles[6][5] = {
            {1,  19, 16, 9,  GREEN},   // 1 绿(低位)
            {5,  15, 12, 9,  RED},     // 2 红
            {9,  11, 9,  8,  GREEN},   // 3 绿
            {13, 8,  6,  8,  RED},     // 4 红
            {17, 6,  4,  7,  RED},     // 5 红
            {21, 3,  1,  7,  RED},     // 6 红(高位)
        };
        for (int i = 0; i < 6; i++) {
            int bx = kCandles[i][0], by = kCandles[i][1];
            int wy = kCandles[i][2], wh = kCandles[i][3];
            uint32_t col = kCandles[i][4];
            px(bx + 1, wy, 1, wh, col);   // 影线(穿过实体)
            px(bx, by, 3, 4, col);        // 实体
        }
    }

    {   // Email 图标:邮件符号(与状态栏同款 Material Symbols,青色)
        lv_obj_t* e = lv_label_create(launcher_items_[1]);
        lv_obj_set_style_text_font(e, &font_material_symbols_20_4, 0);
        lv_obj_set_style_text_color(e, lv_color_hex(0x8ae0f7), 0);
        lv_label_set_text(e, MATERIAL_SYMBOLS_MAIL);
        lv_obj_center(e);
    }
    {   // 娱乐文件夹图标:手柄(Material Symbols,橙;方块 L 形图标移入文件夹页)
        lv_obj_t* gp = lv_label_create(launcher_items_[4]);
        lv_obj_set_style_text_font(gp, &font_material_symbols_20_4, 0);
        lv_obj_set_style_text_color(gp, lv_color_hex(0xf5a263), 0);
        lv_label_set_text(gp, MATERIAL_SYMBOLS_SPORTS_ESPORTS);
        lv_obj_center(gp);
    }
    {   // 播放文件夹图标:♪(Material Symbols 音符)
        lv_obj_t* note = lv_label_create(launcher_items_[2]);
        lv_obj_set_style_text_font(note, &font_material_symbols_20_4, 0);
        lv_obj_set_style_text_color(note, lv_color_hex(C_FG), 0);
        lv_label_set_text(note, MATERIAL_SYMBOLS_MUSIC_NOTE);
        lv_obj_center(note);
    }

    {   // 计时器图标:"计"字
        lv_obj_t* tm = lv_label_create(launcher_items_[3]);
        lv_obj_set_style_text_font(tm, &font_home_cn_16, 0);
        lv_obj_set_style_text_color(tm, lv_color_hex(C_FG), 0);
        lv_label_set_text(tm, "计");
        lv_obj_center(tm);
    }

    {   // 工具文件夹图标:"工"字(青)
        lv_obj_t* nw = lv_label_create(launcher_items_[5]);
        lv_obj_set_style_text_font(nw, &font_home_cn_16, 0);
        lv_obj_set_style_text_color(nw, lv_color_hex(0x8ae0f7), 0);
        lv_label_set_text(nw, "工");
        lv_obj_center(nw);
    }

    app_name_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(app_name_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(app_name_label_, lv_color_hex(C_FG), 0);
    lv_label_set_text(app_name_label_, "");
    lv_obj_align(app_name_label_, LV_ALIGN_BOTTOM_MID, 0, -6);

    LauncherHighlight();

    RefreshClock();
}

void HomeScreen::LauncherHighlight() {
    static const char* kNames[kLauncherCount] = {"金融", "邮件", "播放", "计时", "娱乐", "工具"};
    for (int i = 0; i < kLauncherCount; i++) {
        if (launcher_items_[i] == nullptr) continue;
        lv_obj_set_style_border_color(launcher_items_[i],
            lv_color_hex(i == launcher_sel_ ? C_FG : C_DIM), 0);
        lv_obj_set_style_border_width(launcher_items_[i],
            i == launcher_sel_ ? 2 : 1, 0);
    }
    if (app_name_label_ != nullptr && launcher_sel_ < kLauncherCount) {
        lv_label_set_text_fmt(app_name_label_, "%s · OK 开 · 长按说话", kNames[launcher_sel_]);
    }
}

void HomeScreen::SwitchApp(int idx) {
    // APP 索引:0=金融 1=邮件 2=答案之书 3=播放器(Sonos/HomePod) 4=计时,
    // 5=娱乐文件夹,6=方块,7=Flappy,8=收音机,9=AI新闻,10=播放文件夹,
    // 11=工具文件夹,12=OrangePi
    // (3/8 由播放文件夹进入;2/6/7 由娱乐文件夹进入;9/12 由工具文件夹进入;
    //  启动栏 6 格见 HandleLauncherKey 的 kSlotApp;current_app_ 记真实占用者)
    // 已退出(Leave 后 active_=false)的同一 APP 允许重新进入
    if (idx == current_app_) {
        bool alive = false;
        switch (idx) {
            case 0: alive = FinanceApp::Get().Active(); break;
            case 1: alive = MailApp::Get().Active(); break;
            case 2: alive = AnswerApp::Get().Active(); break;
            case 3: alive = PlayerApp::Get().Active(); break;
            case 4: alive = TimerApp::Get().Active(); break;
            case 5: alive = GamesApp::Get().Active(); break;
            case 6: alive = TetrisApp::Get().Active(); break;
            case 7: alive = FlappyApp::Get().Active(); break;
            case 8: alive = RadioApp::Get().Active(); break;
            case 10: alive = MediaApp::Get().Active(); break;
            case 9: alive = NewsApp::Get().Active(); break;
            case 11: alive = ToolsApp::Get().Active(); break;
            case 12: alive = OpiApp::Get().Active(); break;
            default: break;
        }
        if (alive) return;   // 确实还在运行,跳过
    }
    // 销毁上一个 APP 的对象(释放 LVGL 内存;数据缓存保留在 Poller 侧)
    if (current_app_ >= 0) {
        if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
            switch (current_app_) {
                case 0: FinanceApp::Get().Destroy(); break;
                case 1: MailApp::Get().Destroy(); break;
                case 2: AnswerApp::Get().Destroy(); break;
                case 3: PlayerApp::Get().Destroy(); break;
                case 4: TimerApp::Get().Destroy(); break;
                case 5: GamesApp::Get().Destroy(); break;
                case 6: TetrisApp::Get().Destroy(); break;
                case 7: FlappyApp::Get().Destroy(); break;
                case 8: RadioApp::Get().Destroy(); break;
                case 9: NewsApp::Get().Destroy(); break;
                case 10: MediaApp::Get().Destroy(); break;
                case 11: ToolsApp::Get().Destroy(); break;
                case 12: OpiApp::Get().Destroy(); break;
                default: break;
            }
            lvgl_port_unlock();
        }
    }
    current_app_ = idx;
    lv_obj_t* parent = lv_screen_active();
    // 懒创建 + 进入
    if (idx == 0) {
        FinanceApp::Get().Create(parent, home_font_);
        FinanceApp::Get().Enter();
    } else if (idx == 1) {
        MailApp::Get().Create(parent);
        MailApp::Get().Enter();
    } else if (idx == 2) {
        AnswerApp::Get().Create(parent);
        AnswerApp::Get().Enter();
    } else if (idx == 3) {
        PlayerApp::Get().Create(parent);
        PlayerApp::Get().Enter();
    } else if (idx == 4) {
        TimerApp::Get().Create(parent);
        TimerApp::Get().Enter();
    } else if (idx == 5) {
        GamesApp::Get().Create(parent);
        GamesApp::Get().Enter();
    } else if (idx == 6) {
        TetrisApp::Get().Create(parent);
        TetrisApp::Get().Enter();
    } else if (idx == 7) {
        FlappyApp::Get().Create(parent);
        FlappyApp::Get().Enter();
    } else if (idx == 9) {
        NewsApp::Get().Create(parent);
        NewsApp::Get().Enter();
    } else if (idx == 10) {
        MediaApp::Get().Create(parent);
        MediaApp::Get().Enter();
    } else if (idx == 11) {
        ToolsApp::Get().Create(parent);
        ToolsApp::Get().Enter();
    } else if (idx == 12) {
        OpiApp::Get().Create(parent);
        OpiApp::Get().Enter();
    } else {
        RadioApp::Get().Create(parent);
        RadioApp::Get().Enter();
    }
}

bool HomeScreen::ExitCurrentApp() {
    // 退出即销毁:释放 APP 全部 LVGL 对象(常驻内存归零),
    // 重进时重建(懒创建)。Destroy 幂等(root_ 已空则空操作)。
    if (current_app_ < 0) return false;
    int idx = current_app_;
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        switch (idx) {
            case 0: FinanceApp::Get().Destroy(); break;
            case 1: MailApp::Get().Destroy(); break;
            case 2: AnswerApp::Get().Destroy(); break;
            case 3: PlayerApp::Get().Destroy(); break;
            case 4: TimerApp::Get().Destroy(); break;
            case 5: GamesApp::Get().Destroy(); break;
            case 6: TetrisApp::Get().Destroy(); break;
            case 7: FlappyApp::Get().Destroy(); break;
            case 8: RadioApp::Get().Destroy(); break;
            case 9: NewsApp::Get().Destroy(); break;
            case 10: MediaApp::Get().Destroy(); break;
            case 11: ToolsApp::Get().Destroy(); break;
            case 12: OpiApp::Get().Destroy(); break;
            default: break;
        }
        lvgl_port_unlock();
    }
    current_app_ = -1;
    return true;
}

bool HomeScreen::HandleLauncherKey(int key) {
    if (root_ == nullptr) return false;
    switch (key) {
        case 0:   // 上:前一个(循环)
            launcher_sel_ = (launcher_sel_ + kLauncherCount - 1) % kLauncherCount;
            LauncherHighlight();
            return true;
        case 1:   // 下:后一个(循环)
            launcher_sel_ = (launcher_sel_ + 1) % kLauncherCount;
            LauncherHighlight();
            return true;
        case 2:   // OK:进入选中 APP(切换时销毁上一个,同时最多 1 个 APP 对象)
            // 启动栏槽位 → APP 索引(播放器 3/收音机 8 由播放文件夹进入;
            // 方块 6/Flappy 7/答案之书 2 由娱乐文件夹进入;
            // 新闻 9/OrangePi 12 由工具文件夹进入)
            {
                static const int kSlotApp[kLauncherCount] = {0, 1, 10, 4, 5, 11};
                SwitchApp(kSlotApp[launcher_sel_]);
            }
            return true;
        default:
            return false;
    }
}

void HomeScreen::Show() {
    if (root_ == nullptr || visible_) return;
    ESP_LOGI("home", "Show()");
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
    visible_ = true;
    RefreshClock();
}

void HomeScreen::Hide() {
    if (root_ == nullptr || !visible_) return;
    ESP_LOGI("home", "Hide()");
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    visible_ = false;
}

void HomeScreen::RefreshClock() {
    if (root_ == nullptr) return;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 2025 - 1900) {
        lv_label_set_text(clock_label_, "--:--");
        lv_label_set_text(date_label_, "");
        lv_label_set_text(lunar_label_, "");
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    lv_label_set_text(clock_label_, buf);

    static const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    snprintf(buf, sizeof(buf), "%d月%d日 %s", tm_now.tm_mon + 1, tm_now.tm_mday,
             weekdays[tm_now.tm_wday]);
    lv_label_set_text(date_label_, buf);

    // 农历(表驱动 1900-2099)
    char lunar[24];
    if (SolarToLunarCn(tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                       lunar, sizeof(lunar))) {
        lv_label_set_text(lunar_label_, lunar);
    } else {
        lv_label_set_text(lunar_label_, "");
    }

    // 天气单行:温度 天气 湿度(无数据时提示)
    WeatherSnapshot w = WeatherService::Get().Snapshot();
    if (w.valid) {
        snprintf(buf, sizeof(buf), "%d° %s %d%%", w.temperature_c,
                 w.condition_cn, w.humidity_pct);
        lv_label_set_text(weather_line1_, buf);
    } else {
        lv_label_set_text(weather_line1_, "天气获取中…");
    }

    // GLM 用量(5h 窗口剩余)
    int glm_remaining = GlmService::Get().RemainingPct();
    if (glm_remaining >= 0) {
        snprintf(buf, sizeof(buf), "GLM  %d%%", glm_remaining);
        lv_label_set_text(glm_label_, buf);
    } else {
        lv_label_set_text(glm_label_, "GLM …");
    }

    // DeepSeek 余额(GLM 右侧;无数据保持占位)
    int ds_cents = DeepseekService::Get().BalanceCents();
    if (ds_cents >= 0) {
        snprintf(buf, sizeof(buf), "DS %d.%02d元", ds_cents / 100, ds_cents % 100);
        lv_label_set_text(ds_label_, buf);
    } else {
        lv_label_set_text(ds_label_, "DS …");
    }

    // 最近的提醒/行程
    std::string agenda = AgendaService::Get().Text();
    lv_label_set_text(agenda_label_, agenda.c_str());
}

void HomeScreen::SetMailUnread(int count) {
    if (root_ == nullptr || mail_label_ == nullptr) return;
    if (count > 0) {
        lv_obj_clear_flag(mail_label_, LV_OBJ_FLAG_HIDDEN);
        if (mail_blink_ != nullptr) lv_timer_resume(mail_blink_);   // 闪烁
    } else {
        if (mail_blink_ != nullptr) lv_timer_pause(mail_blink_);
        lv_obj_add_flag(mail_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void HomeScreen::RefreshBattery(int level, bool charging) {
    if (root_ == nullptr) return;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%%s", level, charging ? "+" : "");
    lv_label_set_text(battery_text_, buf);
    int pct = level < 0 ? 0 : (level > 100 ? 100 : level);
    // 框内可用宽约 20px(24 - 2*1 边框 - 2*1 内边距)
    lv_obj_set_width(battery_fill_, 1 + pct * 19 / 100);
}
