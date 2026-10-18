#pragma once

// glm_service —— 智谱 GLM Coding Plan 用量查询(每 5 分钟后台轮询)。
// 端点: GET https://open.bigmodel.cn/api/monitor/usage/quota/limit (Bearer key)
// 显示口径与桌面 codex-usage 挂件一致:5h 窗口剩余百分比。

#include <atomic>
#include <string>

class GlmService {
public:
    static GlmService& Get();

    // 配置 key 与间隔(由统一轮询 task 调用 Poll)。
    void Start(const char* api_key, int interval_minutes);

    // 单次拉取(阻塞,由轮询 task 调用)。
    bool Poll();

    // 5h 窗口剩余百分比;无数据返回 -1。
    int RemainingPct() const { return remaining_pct_.load(); }

private:
    std::string api_key_;
    int interval_minutes_ = 5;
    std::atomic<int> remaining_pct_{-1};
};
