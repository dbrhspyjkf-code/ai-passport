#pragma once

// agenda_service —— hermes /agenda 拉取(下一条行程/提醒,显示就绪文本)。
// 数据源:Apple 日历+提醒(服务端聚合),响应形如
//   {"ok":true,"next":{"text":"行程 21:30 xxx"},"cached_s":107}
//   {"ok":true,"next":null}  —— 今天没有更多安排

#include <atomic>
#include <string>

class AgendaService {
public:
    static AgendaService& Get();

    void Start(const std::string& base_url, int interval_minutes);

    // 请求立即刷新(轮询 task 提前唤醒)。
    void RequestRefresh();

    // 最近两条行程/提醒(各一行,拼好的显示文本);无数据返回空串。
    std::string Text() const;

    // 单次拉取:agenda + 金融三 tab(阻塞,由轮询 task 调用)。
    bool Poll();

private:
    std::atomic<bool> refresh_now_{false};
    std::string url_;
    int interval_minutes_ = 5;
    std::string next_text_;    // 后台 task 写;UI 经 Text() 拷贝
    std::string second_text_;
};
