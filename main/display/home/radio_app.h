#pragma once

// radio_app —— 网络收音机 v3(复古调谐面板 UI,参考 leo-radio 设计)。
// 88-108 MHz 刻度盘+琥珀指针扫动换台;换台播白噪声(调谐声);
// 真实 PCM 峰值电平表。解码转码在服务器端(radio_relay.py),
// 设备 PCM 直通零解码。
// 交互:上/下=换台(指针扫动+自动播放),OK=播放/暂停,长按 OK=退出。

#include <stdint.h>
#include <atomic>

#include "lvgl.h"

class RadioApp {
public:
    static RadioApp& Get();

    // 创建 UI(持 LVGL 锁调用)。
    void Create(lv_obj_t* parent);

    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();

    // 按键路由(board 层持锁调用)。
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK_CLICK

    // ---- 播放任务侧接口 ----
    void StartPlay();
    void StopPlay();
    bool StopRequested() const { return stop_req_.load(); }
    bool TaskRunning() const { return task_running_.load(); }
    void MarkTaskRunning() { task_running_.store(true); }
    void MarkTaskExited() { task_running_.store(false); }
    void OnStreamState(int state);
    // 任务收尾时调用:用户已离开则重建协议
    void RestoreProtocolIfIdle();

    enum StreamState { ST_IDLE = 0, ST_CONNECTING, ST_PLAYING, ST_FAILED };

    static const int kPeaks = 18;   // 电平表条数(UI 定时器访问)

private:
    RadioApp() = default;

    static const int kMaxStations = 8;
    static const int kBandLow = 880;    // 88.0 MHz(0.1MHz 单位)
    static const int kBandHigh = 1080;  // 108.0 MHz
    static const int kTrackLeft = 20;   // 刻度轨道像素范围(面板内)
    static const int kTrackRight = 220;

    // UI
    lv_obj_t* root_ = nullptr;
    lv_obj_t* name_labels_[kMaxStations] = {};    // 台名(预创建,显隐切换)
    lv_obj_t* freq_labels_[kMaxStations] = {};    // 频率读数
    lv_obj_t* st_labels_[6] = {};                 // 状态文字×6
    lv_obj_t* ch_label_ = nullptr;                // "3 / 8"
    lv_obj_t* status_label_ = nullptr;            // 当前显示的状态 label
    lv_obj_t* needle_ = nullptr;          // 调谐指针
    lv_obj_t* glow_ = nullptr;            // 指针光晕
    lv_obj_t* bars_[kPeaks] = {};         // 电平条
    lv_timer_t* vu_timer_ = nullptr;      // 电平刷新
    lv_timer_t* keepalive_timer_ = nullptr;   // 会话期电源定时器保活
    int sel_ = 0;
    bool active_ = false;

    // 播放任务控制
    std::atomic<bool> stop_req_{false};
    std::atomic<bool> task_running_{false};
    std::atomic<bool> protocol_down_{false};

    int FreqOf(int idx) const;            // 电台频率(0.1MHz;0=均布)
    int NeedleX(int freq_tenths) const;
    void ApplyStation(int idx, bool animate);
    void StartNoise();
    void StopNoise();
};
