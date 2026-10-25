#pragma once

// news_service —— AI 新闻查询(hermes /news/ai)。
// GET {base}/news/ai → {"ok":true,"date":"10-24 12:31",
//   "secs":[{"label":"必读","items":[".."]},...],"summary":".."}
// base 形如 http://192.168.1.200:8766(hermes 主服务,OrangePi)。
// 源头是 Pi cron 每天中午生成的午报文件——服务端已做体积约束
// (分区≤4/条目≤6/条目≤120 字),设备侧缓冲与之匹配。

#include <atomic>
#include <string>

class NewsService {
public:
    static NewsService& Get();

    // 配置(由统一轮询 task 调用 Poll)。
    void Start(const std::string& base_url, int interval_minutes);

    // 单次查询(阻塞;失败保留旧值)。
    void Poll();

    // 最近一次 Poll 是否有有效数据。
    bool Valid() const { return valid_; }

    // 数据版本:每次成功 Poll 后 +1,界面据此判断是否需要重绘。
    uint32_t Version() const { return version_.load(); }

    // 新闻 APP 按 OK 手动刷新:请求轮询任务尽快拉取一次。
    void RequestNow() { force_now_.store(true); }
    bool ForcePending() const { return force_now_.load(); }
    bool ConsumeForce() { return force_now_.exchange(false); }

    // 日期头(如 "10-24 12:31";无数据为空串)。
    const char* Date() const { return date_; }
    // 一句话总结(可为空)。
    const char* Summary() const { return summary_; }
    // 条目总数(不含总结)。
    int ItemCount() const { return total_; }
    // 取第 idx 条(idx < ItemCount);label=分区名。返回 false 越界。
    bool GetItem(int idx, char* label, size_t label_cap,
                 char* text, size_t text_cap) const;

private:
    NewsService() = default;

    static const int kMaxSecs = 3;
    static const int kMaxItemsPerSec = 5;
    struct Sec {
        char label[12];
        char items[kMaxItemsPerSec][96];
        int count;
    };
    Sec secs_[kMaxSecs] = {};
    int sec_count_ = 0;
    int total_ = 0;
    char date_[20] = {};
    char summary_[160] = {};
    bool valid_ = false;

    std::atomic<bool> force_now_{false};
    std::atomic<uint32_t> version_{0};
    std::string base_url_;
    int interval_minutes_ = 30;
};
