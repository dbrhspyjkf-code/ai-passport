#pragma once

// poller_http —— 统一 HTTP 响应缓冲(8KB)。
// 仅限"poller 任务内串行执行"的拉取方共用:天气/金融/邮件/新闻/OrangePi
// (各自专属缓冲曾合计 ~16KB 常驻,把开机内存谷压到 532B)。
// 禁止 APP 任务直接使用:播放器等 APP 拉取与 poller 收尾轮存在 ≤1s
// 竞态窗口(APP 切入后仲裁 Stop 最迟下一秒生效),需自留缓冲。

namespace poller_http {

inline char body[8192] = {};

}   // namespace poller_http
