#pragma once

// pod_app —— HomePod 音箱控制(经 Home Assistant REST API,参考 M5-DIAL)。
// 精简版:无专辑图。显示 曲名/艺术家/播放状态/音量条/进度条;
// 控制 上/下=音量±5,OK=播放/暂停,长按上/下=上一首/下一首,OK 长按=退出。
// 凭证在 pod_secrets.h(gitignored)。

#include <atomic>

#include "lvgl.h"

class PodApp {
public:
    static PodApp& Get();

    void Create(lv_obj_t* parent);
    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK_CLICK 3=OK_LONG 4=UP_LONG 5=DOWN_LONG

private:
    PodApp() = default;

    static void TaskEntry(void* arg);
    void Run();
    bool FetchStatus();
    void SendCommand(const char* service);
    void ApplyVolume(int delta);
    void UpdateUi();
    double ComputeTruePos();   // 真实位置(校准/本地推进);-1=不可用

    std::atomic<bool> running_{false};
    std::atomic<int> cmd_{0};
    char title_[96];
    char artist_[64];
    char state_[16];
    float volume_ = 0;
    double ha_pos_ = 0;
    double ha_at_epoch_ = 0;
    double last_good_pos_ = 0;       // 最近一次新鲜推送的位置(秒)
    double last_good_time_ = 0;
    double display_pos_ = 0;         // 显示位置(匀速推进,校准只在偏差>5s 时纠正)
    char last_title_[96] = {};       // 切歌检测      // 收到该推送的时刻(epoch)
    float dur_min_ = 0;
    bool valid_ = false;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* artist_label_ = nullptr;
    lv_obj_t* vol_bar_ = nullptr;
    lv_obj_t* vol_label_ = nullptr;
    lv_obj_t* pos_label_ = nullptr;
    lv_obj_t* pos_bar_ = nullptr;
    lv_timer_t* ui_timer_ = nullptr;   // 500ms 进度刷新(M5-DIAL 同款)
    bool active_ = false;
};
