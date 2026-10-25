#pragma once

// poller_http —— 单 poller 任务串行复用的 HTTP 响应缓冲。
// mail/news/opi 全部只在 poller Loop 里同步拉取,天然串行,共用一块
// 缓冲即可(此前每服务各留 3-4KB 静态缓冲,C3 的 .bss 挤到开机
// bad_alloc 边缘)。需要新数据源时直接复用,不要再开自己的缓冲。

namespace poller_http {

inline char body[4096] = {};

}   // namespace poller_http
