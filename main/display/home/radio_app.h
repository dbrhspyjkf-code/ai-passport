#pragma once

// radio_app —— 网络收音机 v3(复古调谐面板 UI,参考 leo-radio 设计)。
// 屏中央收音机图标+音量条(root 绘制事件直绘,零对象)。解码转码在服务器端
// (radio_relay.py),设备 PCM 直通零解码。
// 交互:短按上/下=音量,长按上/下=换台,OK=播放/暂停,长按 OK=退出。

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
    // 0=UP 短按(音量+) 1=DOWN 短按(音量-) 2=OK(播放/暂停)
    // 3=UP 长按(上一台) 4=DOWN 长按(下一台)
    void HandleKey(int key);

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
    lv_obj_t* freq_labels_[kMaxStations] = {};    // 频率读数(网络台="网络电台")
    lv_obj_t* st_labels_[6] = {};                 // 状态文字×6
    lv_obj_t* status_label_ = nullptr;   // 当前显示的状态 label
    lv_obj_t* needle_ = nullptr;          // 调谐指针
    lv_obj_t* glow_ = nullptr;            // 指针光晕
    lv_timer_t* start_timer_ = nullptr;   // 换台/播放:等旧任务退出后自动续播
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
    void ChangeStation(int dir);             // ±1:指针扫动+停旧流+防抖续播
    void AdjustVolume(int delta);            // 硬件音量 ±,刷新音量条
    void ResetUiRefs();                      // 清空全部 UI 对象引用(Destroy/Create 失败回退共用)
    void ScheduleStart(uint32_t delay_ms);   // 延时启动/换台续播(持 LVGL 锁调用)
    void CancelStartTimer();                 // 持 LVGL 锁调用
};
