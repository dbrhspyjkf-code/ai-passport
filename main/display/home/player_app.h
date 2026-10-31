#pragma once

// player_app —— Sonos + HomePod 统一播放控制器(tab 切换)。
// 顶部两个 tab,双击 OK 切换;数据/命令路由到当前播放器。
// 按键: 短按上/下=音量±5,长按上/下=切歌,OK=播放/暂停,
//       双击 OK=切 tab,长按 OK=退出(board 层)。
// 歌词: 切歌时经 radio_relay(8769) /lyrics 代理拉 LRC(MA 主源+LRCLIB
// 兜底,服务端缓存),TickProgress 按实时位置推进双行(当前亮+下一暗)。

#include <atomic>
#include <cstdint>

#include "lvgl.h"

#include "sonos_secrets.h"
#include "pod_secrets.h"

class PlayerApp {
public:
    static PlayerApp& Get();

    void Create(lv_obj_t* parent);
    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK 3=OK_DOUBLE 4=UP_LONG 5=DOWN_LONG

private:
    PlayerApp() = default;

    struct PlayerCfg {
        const char* base_url;
        const char* token;
        const char* entity_id;
        const char* tab_name;
    };
    static constexpr int kPlayers = 2;

    static void TaskEntry(void* arg);
    void Run();
    bool FetchStatus(int idx);
    void SendCommand(int idx, const char* service);
    void ApplyVolume(int idx, int delta);
    bool FetchCover(int idx);
    void ResetCover();
    void FetchLyrics(int idx);   // 切歌/切 tab 时拉 LRC(radio_relay /lyrics)
    void UpdateUi();
    void TickProgress();   // 500ms 进度推进(钟表式)
    void SwitchTab(int idx);

    // 每个播放器的数据
    struct PlayerState {
        char title[96];
        char artist[64];
        char picture_url[192];
        char state[16];
        float volume = 0;
        double ha_pos = 0;          // HA 上报位置(秒)
        double ha_at_epoch = 0;     // 上报时刻
        double last_good_pos = 0;   // 最近一次新鲜推送的位置
        double last_good_time = 0;
        double display_pos = 0;     // 显示位置
        int64_t anchor_us_ = 0;     // 单调时钟锚(免疫 SNTP/时区偏差)
        float dur_min = 0;
        char last_title[96] = {};
        char last_picture_url[192] = {};
        bool valid = false;
    };
    PlayerState st_[kPlayers];

    std::atomic<bool> running_{false};
    std::atomic<int> cmd_target_{0};   // 命令路由的播放器
    std::atomic<int> cmd_{0};          // 0=无 1=play_pause 2=vol_up 3=vol_down 4=prev 5=next

    std::atomic<int> cur_{0};   // 当前 tab：按键/UI 与 Player 网络任务之间安全共享
    lv_obj_t* root_ = nullptr;
    lv_obj_t* tab_lbl_[kPlayers] = {};   // 纯文本 tab(金融 app 同款)
    lv_obj_t* cover_img_ = nullptr;
    lv_image_dsc_t cover_dsc_ = {};
    uint8_t* cover_data_ = nullptr;    // 仅进入播放器后按需申请 5120B LVGL I8
    std::atomic<bool> cover_requested_{false};
    int cover_retries_left_ = 0;       // 外部切歌时，等待 HA 的 entity_picture 同步
    int cover_player_ = -1;            // cover_data_ 所属 tab，防止 Sonos 图显示到 HomePod
    int64_t cover_retry_due_us_ = 0;
    bool cover_ready_ = false;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* artist_label_ = nullptr;
    lv_obj_t* lyric_now_ = nullptr;    // 歌词当前行(仅一行)
    // 歌词紧凑存储: 文本堆缓冲 + 升序时间/偏移数组(TickProgress 读,
    // player 任务写;发布走 lvgl 锁,lyr_count_=0 即失效)
    static constexpr int kMaxLyrLines = 96;
    static constexpr int kLyrBufBytes = 3072;
    uint8_t* lyr_buf_ = nullptr;       // LRC 行文本(进 APP 预分配,退出释放)
    float lyr_t_[kMaxLyrLines] = {};   // 行起始秒(升序)
    uint16_t lyr_off_[kMaxLyrLines] = {};  // 行文本在 lyr_buf_ 内偏移
    int lyr_count_ = 0;
    int lyr_idx_ = -1;                 // 当前行(-1 无/已清,-2 强制重设)
    int lyr_player_ = -1;              // 词属于哪个 tab(切 tab 防串词)
    char lyr_key_title_[96] = {};      // 拉取时的曲名快照(陈旧判据)
    std::atomic<bool> lyr_requested_{false};
    // 同曲有限重试(中继冷查询偶发超时后,20s 后再试,最多 3 次;
    // 二次起中继缓存已热秒回)。仅 player 任务读写。
    char lyr_attempt_title_[96] = {};
    int lyr_attempts_ = 0;
    int64_t lyr_retry_due_us_ = 0;
    lv_obj_t* vol_label_ = nullptr;
    lv_obj_t* pos_bar_ = nullptr;
    lv_obj_t* pos_label_ = nullptr;
    lv_obj_t* prog_label_ = nullptr;   // 进度时间
    lv_timer_t* ui_timer_ = nullptr;
    bool active_ = false;
};
