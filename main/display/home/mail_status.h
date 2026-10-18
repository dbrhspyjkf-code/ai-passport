#pragma once

// mail_status —— hermes email-status-server 未读邮件查询。
// GET {base}/api/email/status → {"unread": N, "folders": [...]}
// base 形如 http://192.168.1.200:8768(独立服务,非 8766 主服务)。

#include <atomic>
#include <string>

class MailStatus {
public:
    static MailStatus& Get();

    // 配置(由统一轮询 task 调用 Poll)。
    void Start(const std::string& base_url, int interval_minutes);

    // 单次查询(阻塞)。
    void Poll();

    // 未读邮件数;无数据返回 -1。
    int Unread() const { return unread_.load(); }

    // 取走新邮件信息(一次性):有新邮件返回 true 并填充发件人/主题。
    bool TakeNewMail(char* from, size_t from_cap, char* subject, size_t subject_cap);

    // 未读邮件条数(最近一次 Poll 缓存)。
    int MessageCount() const { return msg_count_; }
    // 取第 idx 封(idx < MessageCount);返回 false 越界。
    bool GetMessage(int idx, char* from, size_t from_cap,
                    char* subject, size_t subject_cap,
                    char* preview, size_t preview_cap) const;

private:
    MailStatus() = default;

    std::atomic<int> unread_{-1};
    bool has_new_ = false;
    char new_from_[64] = {};
    char new_subject_[128] = {};

    struct Msg { char from[72]; char subject[84]; char preview[304]; };
    Msg msgs_[10];
    int msg_count_ = 0;
    std::string base_url_;
    int interval_minutes_ = 2;
};
