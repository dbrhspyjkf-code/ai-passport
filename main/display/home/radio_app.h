#pragma once

// radio_app —— 网络收音机 v4(复古调谐面板 UI + 输出选择)。
// 屏中央收音机图标+音量条(root 绘制事件直绘,零对象)。解码转码在服务器端
// (radio_relay.py),设备 PCM 直通零解码。
// 输出选择(2026-11-01,参考 Tab5 收音机): 本机喇叭 / 客厅 Sonos /
// 起居室 HomePod。外部输出走中继 /radio/<key>/play_on + /cmd 端点,
// 本机不播音(NVS 持久化,电台列表首行切换)。
// 交互:短按上/下=音量,长按上/下=频道列表(列表内短按上下选择,OK 播放
// 并返回;列表首行=输出切换),OK=播放/暂停,长按 OK=退出。

#include <stdint.h>
#include <atomic>

#include "lvgl.h"

class RadioApp {
public:
    static RadioApp& Get();

    void Create(lv_obj_t* parent);

    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();

    // 绘制回调读取(音量条:外部模式用 ext_vol_ 而非 codec)
    int out_sel_ = 0;
    int ext_vol_ = 50;

    // 按键路由(board 层持锁调用)。
    // 0=UP 短按(音量+) 1=DOWN 短按(音量-) 2=OK(播放/暂停)
    // 3=UP 长按(频道列表) 4=DOWN 长按(频道列表)
    void HandleKey(int key);

    // ---- 播放任务侧接口 ----
    void StartPlay();
    void StopPlay();
    bool StopRequested() const { return stop_req_.load(); }
    bool TaskRunning() const { return task_running_.load(); }
    void MarkTaskRunning() { task_running_.store(true); }
    void MarkTaskExited() { task_running_.store(false); }
    void OnStreamState(int state);
    void RestoreProtocolIfIdle();

    enum StreamState { ST_IDLE = 0, ST_CONNECTING, ST_PLAYING, ST_FAILED };

private:
    RadioApp() = default;

    static const int kMaxStations = 14;
    static const int kBandLow = 880;
    static const int kBandHigh = 1080;
    static const int kTrackLeft = 20;
    static const int kTrackRight = 220;
    static const int kListTop = 28;
    static const int kListRow = 22;

    // ── 输出选择(参考 Tab5:本机/Sonos/HomePod,走中继 play_on/cmd) ──
    struct RadioOutput { const char* name; const char* pid; };
    static const RadioOutput kOutputs[3];   // 本机/客厅/起居室
    static const int kOutputCount = 3;
    // out_sel_/ext_vol_ 已提升为 public(绘制回调读取)
    std::atomic<bool> is_playing_{false};   // 用户意图:正在播(OK 键切换的唯一依据)
    lv_obj_t* out_label_ = nullptr;          // 主屏输出指示(非本机时显示)
    bool ExtHttpGet(const char* path);       // LAN GET(3s 超时)
    bool ExtHttpGetTimeout(const char* path, int ms);  // 带超时(play_on 用 6s)
    void ExtPlay(int station);               // 投到外部播放器
    void ExtStop();                          // 停外部播放
    void ExtSetVolume(int vol);              // 外部音量(0-100)
    void CycleOutput();                      // OK on 输出行:循环切换
    void RefreshOutputUi();                  // 更新指示文字

    // UI
    lv_obj_t* root_ = nullptr;
    lv_obj_t* name_labels_[kMaxStations] = {};
    lv_obj_t* st_labels_[6] = {};
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* needle_ = nullptr;
    lv_obj_t* glow_ = nullptr;
    lv_obj_t* list_panel_ = nullptr;
    lv_obj_t* list_bar_ = nullptr;
    lv_obj_t* out_row_label_ = nullptr;     // 列表首行"输出: XXX"
    bool list_open_ = false;
    int list_sel_ = 0;                       // -1=输出行,0+=电台
    lv_timer_t* start_timer_ = nullptr;
    lv_timer_t* keepalive_timer_ = nullptr;
    int sel_ = 0;
    bool active_ = false;

    // 播放任务控制
    std::atomic<bool> stop_req_{false};
    std::atomic<bool> task_running_{false};
    std::atomic<bool> protocol_down_{false};

    int FreqOf(int idx) const;
    int NeedleX(int freq_tenths) const;
    void ApplyStation(int idx, bool animate);
    void ChangeStation(int dir);
    void PlayStation(int idx);
    void OpenStationList();
    void CloseStationList();
    void MoveListSel(int d);
    void UpdateListHighlight();
    void AdjustVolume(int delta);
    void ResetUiRefs();
    void ScheduleStart(uint32_t delay_ms);
    void CancelStartTimer();
};
