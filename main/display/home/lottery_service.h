#pragma once

// lottery_service —— 双色球开奖查询(hermes /api/ssq/history)。
// 数据源:官方 cwl.gov.cn(服务端每日 21:35 刷新)。
// 响应 ~30KB(100 期含奖项),超共享缓冲—— Poll 内用字符串提取
// (非 cJSON)从原始文本逐期抓 code/reds/blue/sales/pool,避开 JSON 缓冲上限。
// 仅 APP 打开/OK 时经 poller force 路径拉取。

#include <atomic>
#include <string>

class LotteryService {
public:
    static LotteryService& Get();

    void Start(const std::string& base_url);
    void Poll();

    bool Valid() const { return valid_; }
    uint32_t Version() const { return version_.load(); }

    void RequestNow() { force_now_.store(true); }
    bool ForcePending() const { return force_now_.load(); }
    bool ConsumeForce() { return force_now_.exchange(false); }

    static const int kMaxIssues = 30;
    struct Issue {
        char code[8];       // "2026125"
        char date[6];       // "11-01"
        int  reds[6];
        int  blue;
        long sales;         // 销售额(元)
        long pool;          // 奖池(元)
        int  prize1_count;  // 一等奖注数
        long prize1_money;  // 一等奖奖金(元)
    };
    int Count() const { return count_; }
    const Issue& Get(int i) const { return issues_[i]; }

private:
    LotteryService() = default;

    Issue issues_[kMaxIssues] = {};
    int count_ = 0;
    bool valid_ = false;

    std::atomic<bool> force_now_{false};
    std::atomic<uint32_t> version_{0};
    std::string base_url_;
};
