#pragma once

// deepseek_service —— DeepSeek API 账户余额查询(随 GLM 同节奏轮询)。
// 端点: GET {hermes}/api/deepseek/balance →
//   {"ok": true, "currency": "CNY", "total_balance": "49.93", ...}
// 主页 GLM 右侧单行显示:"DS 49.93元"。

#include <atomic>

class DeepseekService {
public:
    static DeepseekService& Get();

    // 单次拉取(阻塞,由轮询 task 调用)。
    bool Poll();

    // 余额(分);无数据返回 -1。货币固定 CNY(DeepSeek 账户币种)。
    int BalanceCents() const { return balance_cents_.load(); }

private:
    std::atomic<int> balance_cents_{-1};
};
