#pragma once

// home_screen —— 开机/待机像素名片主页(复刻社区玩法版面风格,原创实现)。
//
// 版面(240x320):
//   顶部状态栏  信号 · PASSPORT ····· 电量%
//   左栏        像素头像 + 名字 + 签名
//   右栏        点阵大字时钟 HH:MM / 日期 · 星期 / 位置 / 分隔线 / 天气
//   中下        像素城市横幅
//   底部按键栏  OK 对话 · 上 音量 · 下 静音
//
// 显示策略:idle 状态显示主页并遮盖小智默认 UI(表情/聊天);
// 其他状态(听/说/激活/配网)自动隐藏主页,小智原生界面照常。
// 时钟由 LvglDisplay 的状态栏刷新驱动;天气由 WeatherService 后台拉取。

// 天气:hermes 局域网 /weather 端点(服务端已配城市,毫秒级响应)
#define WEATHER_API_URL "http://192.168.1.200:8766/weather"

// 智谱 GLM Coding Plan 的 API Key(用量显示用;私有信息,仅本地编译)
#define GLM_API_KEY "b6194f413f744a45aef6fe8f42514610.Eiw14xe5cnScOZG4"

// hermes 局域网服务(下一条行程/提醒 /agenda;改 IP 同步改这里)
#define HOME_AGENDA_BASE_URL "http://192.168.1.200:8766"

// hermes 主服务 /mail/status(聚合 8768,与 GLM 同源已验证)
#define HOME_MAIL_BASE_URL "http://192.168.1.200:8766"

// hermes 主服务 /news/ai(AI 新闻 APP;源头=Pi cron 午报文件缓冲)
#define HOME_NEWS_BASE_URL "http://192.168.1.200:8766"

// hermes 主服务 /system/status(OrangePi APP;与 Mac Übersicht widget 同源)
#define HOME_OPI_BASE_URL "http://192.168.1.200:8766"

#include <string>

#include "lvgl.h"

class GlmService;
class WeatherService;

class HomeScreen {
public:
    const lv_font_t* HomeFont() const { return home_font_; }
    static HomeScreen& Get();

    // 创建 LVGL 对象(必须在持 LVGL 锁时调用)。content_font 为正文字体。
    void Create(lv_obj_t* parent, const lv_font_t* content_font);

    // 显示/隐藏主页(带 LVGL 锁由调用方持有)。
    void Show();
    void Hide();
    bool Visible() const { return visible_; }

    // 每分钟/状态变化时刷新时钟与电量(持锁调用)。
    void RefreshClock();
    void RefreshBattery(int level, bool charging);
    void SetMailUnread(int count);

    // Launcher:主页底部 APP 栏。返回 true 表示已消费该键。
    // key: 0=UP 1=DOWN 2=OK_CLICK 3=OK_LONG
    bool HandleLauncherKey(int key);
    void SwitchApp(int idx);   // 切换 APP(销毁上一个)
    bool ExitCurrentApp();     // 退出即销毁当前 APP(释放常驻内存),无 APP 则 false
    void LauncherHighlight();
    int launcher_selected() const { return launcher_sel_; }

private:
    lv_timer_t* blink_timer_ = nullptr;   // 头像眨眼(10s)
    lv_obj_t* avatar_img_ = nullptr;   // 头像图片(眨眼切换源)
    bool blink_closed_ = false;
    int current_app_ = -1;   // 当前 APP 索引(-1=无);进新 APP 销毁旧的
    const lv_font_t* home_font_ = nullptr;   // 懒创建 APP 时传给 FinanceApp
    HomeScreen() = default;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* lunar_label_ = nullptr;     // 农历日期(表驱动)
    lv_obj_t* mail_label_ = nullptr;
    lv_timer_t* mail_blink_ = nullptr;       // 未读邮件信封图标(有未读才显示)
    lv_obj_t* app_name_label_ = nullptr;   // 横线下方:选中 APP 名
    static const int kLauncherCount = 6;   // 金融/Email/播放文件夹/计时/娱乐文件夹/工具文件夹
    lv_obj_t* launcher_items_[kLauncherCount] = {};
    int launcher_sel_ = 0;
    lv_obj_t* battery_text_ = nullptr;    // "70%"(含充电 + 后缀)
    lv_obj_t* battery_frame_ = nullptr;   // 像素电池外框
    lv_obj_t* battery_fill_ = nullptr;    // 电量填充(宽度按百分比)
    lv_obj_t* weather_line1_ = nullptr;   // 温度 + 天气 + 湿度
    lv_obj_t* glm_label_ = nullptr;       // GLM 5h 窗口剩余百分比
    lv_obj_t* ds_label_ = nullptr;        // DeepSeek 余额(GLM 右侧,"DS 49.93元")
    lv_obj_t* opi_label_ = nullptr;       // OrangePi 状态(GLM 下方,"CPU12% 内存69% 温度46°")
    lv_obj_t* agenda_label_ = nullptr;    // 最近的提醒/行程文本
    bool visible_ = false;
};
