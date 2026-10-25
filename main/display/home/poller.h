#pragma once

// poller —— 统一后台轮询 task:agenda/金融/天气/GLM/邮件/AI 新闻顺序拉取。
// 单 task 替代四个常驻 task,省 ~20KB 常驻栈;峰值串行不叠加。
// 周期 5 分钟(天气失败自动重试逻辑在其内部);支持 RequestRefresh 提前唤醒。

#include <atomic>
#include <cstdint>
#include <string>

class Poller {
public:
    static Poller& Get();

    // 创建后台轮询 task(幂等)。须在各项 Service::Start 配置之后调用。
    void Start();
    void Pause();   // 播放器等内存大户使用期间挂起轮询
    void Resume();
    void Stop();    // 删除任务(真释放栈;Start 重建)

    // 请求立即执行一轮(提前唤醒)。
    void RequestRefresh();

private:
    Poller() = default;
    void Loop();
    void Beat();           // 更新看门狗心跳(每次 HTTP 调用前/等待循环内)
    void WatchdogLoop();   // 心跳停滞=HTTP 黑洞吊死 → esp_restart() 自愈

    std::atomic<bool> paused_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> refresh_now_{false};
    std::atomic<uint32_t> beat_ms_{0};   // 最近一次轮询进展的时间戳(看门狗心跳)
    std::atomic<uint32_t> generation_{0};   // Stop 时 +1:老任务据此严格退出,避免 Stop 后快速 Start 导致双任务永生
};

// 一次性写入各数据源的配置(在 Poller::Start 之前调用)。
void PollerConfig(const std::string& weather_url, int weather_interval_min,
                  const char* glm_key, int glm_interval_min,
                  const std::string& agenda_base_url, int agenda_interval_min,
                  const std::string& mail_base_url, int mail_interval_min,
                  const std::string& news_base_url, int news_interval_min,
                  const std::string& opi_base_url);
