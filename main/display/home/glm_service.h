#pragma once

// glm_service —— 智谱 GLM Coding Plan 用量查询(每 5 分钟后台轮询)。
// 端点: hermes /glm/quota(缓存,含 5h/7d 窗口 + 重置时间)。
// 主页显示: "GLM 5H 77%  7D 55% RT 01:41"

#include <atomic>
#include <string>
#include <cstdint>

class GlmService {
public:
    static GlmService& Get();

    void Start(const char* api_key, int interval_minutes);
    bool Poll();

    int RemainingPct() const { return remaining_pct_.load(); }
    int Pct7d() const { return pct_7d_.load(); }
    // 5h 窗口重置时刻(epoch ms);0=未知
    int64_t ResetMs() const { return reset_ms_.load(); }
    // 格式化重置倒计时("HH:MM");过期/未知返回空串
    static void FormatReset(char* buf, int cap);

private:
    std::string api_key_;
    int interval_minutes_ = 5;
    std::atomic<int> remaining_pct_{-1};
    std::atomic<int> pct_7d_{-1};
    std::atomic<int64_t> reset_ms_{0};
    // 时区免疫锚点: Poll 时记录 (reset - time()) 与单调钟
    std::atomic<int64_t> anchor_reset_minus_now_ms_{0};
    std::atomic<int64_t> anchor_mono_ms_{0};
};
