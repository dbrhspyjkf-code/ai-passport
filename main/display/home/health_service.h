#pragma once

// health_service —— 健康数据查询(hermes /api/health)。
// GET {base}/api/health → {"ok":true,"updated_at":"...","types":{
//   "<type>":{"value":N,"unit":"..","at":"..","kind":".."}, ...}}
// 数据由 iPhone 快捷指令推送至 hermes 聚合(与 M5Tab5 app_health 同源)。
// 拉取时机:仅 APP 打开/OK 时经 poller force 路径(无后台周期)。

#include <atomic>
#include <string>

class HealthService {
public:
    static HealthService& Get();

    void Start(const std::string& base_url);
    void Poll();

    bool Valid() const { return valid_; }
    uint32_t Version() const { return version_.load(); }

    void RequestNow() { force_now_.store(true); }
    bool ForcePending() const { return force_now_.load(); }
    bool ConsumeForce() { return force_now_.exchange(false); }

    // 更新时刻("HH:MM",无数据为空)
    const char* UpdatedAt() const { return updated_at_; }

    static const int kMetricCount = 10;
    struct Metric {
        const char* type;    // hermes types 键(静态)
        const char* name;    // 中文短名
        const char* unit;    // 显示单位
        float value;         // 0=缺
        bool present;
    };
    const Metric* metrics() const { return metrics_; }

private:
    HealthService() = default;

    Metric metrics_[kMetricCount] = {
        {"step_count",                "步数",   "步",   0, false},
        {"walking_running_distance",  "距离",   "km",   0, false},
        {"active_energy_burned",      "活动",   "kcal", 0, false},
        {"heart_rate",                "心率",   "bpm",  0, false},
        {"resting_heart_rate",        "静息",   "bpm",  0, false},
        {"heart_rate_variability",    "变异",   "ms",   0, false},
        {"blood_oxygen_saturation",   "血氧",   "%",    0, false},
        {"respiratory_rate",          "呼吸",   "次/分", 0, false},
        {"flights_climbed",           "爬楼",   "层",   0, false},
        {"sleep_analysis",            "睡眠",   "",     0, false},   // min→h:mm
    };
    char updated_at_[8] = {};   // "20:03"
    bool valid_ = false;

    std::atomic<bool> force_now_{false};
    std::atomic<uint32_t> version_{0};
    std::string base_url_;
};
