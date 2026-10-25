#pragma once

// opi_service —— OrangePi 系统状态查询(hermes /system/status)。
// GET {base}/system/status → {"ok":true,"host":{...},"load":{...},
//   "memory":{...},"disk":[{...}],"thermal":{...}}
// 同一端点亦服务 Mac Übersicht 桌面 widget(orange-pi.widget)。
// 拉取时机:主页状态行随 GLM 节奏(5 分钟,poller);OrangePi APP
// 打开/OK 时经 force 路径立即拉取。

#include <atomic>
#include <string>

class OpiService {
public:
    static OpiService& Get();

    // 配置(base 形如 http://192.168.1.200:8766;无周期,仅按需)。
    void Start(const std::string& base_url);

    // 单次查询(阻塞;失败保留旧值)。
    void Poll();

    bool Valid() const { return data_.valid; }

    // 数据版本:每次成功 Poll 后 +1,界面据此判断是否需要重绘。
    uint32_t Version() const { return version_.load(); }

    // APP 打开/OK 刷新:请求轮询任务尽快拉取一次。
    void RequestNow() { force_now_.store(true); }
    bool ForcePending() const { return force_now_.load(); }
    bool ConsumeForce() { return force_now_.exchange(false); }

    struct Data {
        char hostname[24] = {};
        char ip[18] = {};
        char kernel[20] = {};
        char uptime[20] = {};    // "18d 20h 21m"
        float load1 = 0, load5 = 0, load15 = 0;
        float cpu_percent = 0;   // 0..100
        float mem_percent = 0;   // 0..100
        int mem_used_mb = 0, mem_total_mb = 0;
        int swap_percent = -1;   // -1=无 swap
        float disk_percent = 0;  // 0..100(根分区)
        float disk_used_gb = 0, disk_total_gb = 0;
        float cpu_temp_c = -1;   // -1=无读数
        int fan_rpm = -1;        // -1=被动散热
        bool valid = false;
    };
    const Data& data() const { return data_; }

private:
    OpiService() = default;

    Data data_;
    std::atomic<bool> force_now_{false};
    std::atomic<uint32_t> version_{0};
    std::string base_url_;
};
