#pragma once

// poller —— 统一后台轮询 task:agenda/金融三 tab/天气/GLM 顺序拉取。
// 单 task 替代四个常驻 task,省 ~20KB 常驻栈;峰值串行不叠加。
// 周期 5 分钟(天气失败自动重试逻辑在其内部);支持 RequestRefresh 提前唤醒。

#include <atomic>
#include <string>

class Poller {
public:
    static Poller& Get();

    // 创建后台轮询 task(幂等)。须在各项 Service::Start 配置之后调用。
    void Start();

    // 请求立即执行一轮(提前唤醒)。
    void RequestRefresh();

private:
    Poller() = default;
    void Loop();

    std::atomic<bool> running_{false};
    std::atomic<bool> refresh_now_{false};
};

// 一次性写入各数据源的配置(在 Poller::Start 之前调用)。
void PollerConfig(const std::string& weather_url, int weather_interval_min,
                  const char* glm_key, int glm_interval_min,
                  const std::string& agenda_base_url, int agenda_interval_min,
                  const std::string& mail_base_url, int mail_interval_min);
