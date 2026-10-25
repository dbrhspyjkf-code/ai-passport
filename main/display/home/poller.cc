// poller.cc —— 统一后台轮询 task。
#include "poller.h"
#include "assets/lang_config.h"
#include "application.h"
#include "board.h"
#include "display.h"

#include <chrono>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "agenda_service.h"
#include "deepseek_service.h"
#include "finance_app.h"
#include "glm_service.h"
#include "home_screen.h"
#include "mail_status.h"
#include "news_service.h"
#include "weather_service.h"

static const char* TAG = "poller";

// 各项配置(由 lvgl_display 创建主页时写入)
static std::string s_weather_url;
static int s_weather_interval = 10;
static std::string s_glm_key;
static int s_glm_interval = 5;
static std::string s_agenda_url;
static int s_agenda_interval = 5;
static std::string s_mail_url;
static int s_mail_interval = 2;
static std::string s_news_url;
static int s_news_interval = 30;

void PollerConfig(const std::string& weather_url, int weather_min,
                  const char* glm_key, int glm_min,
                  const std::string& agenda_url, int agenda_min,
                  const std::string& mail_url, int mail_min,
                  const std::string& news_url, int news_min) {
    s_weather_url = weather_url;
    s_weather_interval = weather_min;
    s_glm_key = glm_key;
    s_glm_interval = glm_min;
    s_agenda_url = agenda_url;
    s_agenda_interval = agenda_min;
    s_mail_url = mail_url;
    s_mail_interval = mail_min;
    s_news_url = news_url;
    s_news_interval = news_min;
}

Poller& Poller::Get() {
    static Poller instance;
    return instance;
}

void Poller::RequestRefresh() {
    ESP_LOGI(TAG, "refresh requested");
    refresh_now_.store(true);
}

void Poller::Pause() {
    if (!paused_.exchange(true)) ESP_LOGI(TAG, "paused (foreground app holds network)");
}
// Loop 尾部:vTaskDelete 自删(Stop 释放栈的机制)
void Poller::Stop() {
    // 真停:代际 +1 且 running_=false 后,Loop 在下一个检测点(≤1s)退出并自删
    // 任务,释放 12KB 栈(Pause 只是空转仍占内存,播放器/对话需要这块内存)。
    // generation 必须自增:否则 Stop 后立刻 Start 会让老任务看到 running_
    // 重新为真而永不退出,造成两个 poller 并存。
    generation_.fetch_add(1);
    running_.exchange(false);
}
void Poller::Resume() {
    if (paused_.exchange(false)) ESP_LOGI(TAG, "resumed");
}

// 看门狗心跳:任何可能阻塞的 HTTP 调用前、每轮开始、空闲等待循环内都要打。
void Poller::Beat() {
    beat_ms_.store((uint32_t)(esp_timer_get_time() / 1000));
}

// 心跳停滞判定吊死。典型场景:Wi-Fi 瞬断后 esp_http_client 落入 lwIP/ARP
// 黑洞永久阻塞(timeout_ms 覆盖不到),poller 单任务随之吊死,所有数据源
// 永久停更。整机重启是对"任务卡在系统调用里"唯一可靠的恢复:xiaozhi
// 与 Wi-Fi 秒级重连,代价可接受。
void Poller::WatchdogLoop() {
    const uint32_t kTimeoutMs = 150000;   // 正常一轮最坏 ~90s(各服务超时串行)
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        if (!running_.load() || paused_.load()) continue;   // 主动暂停/停止不算吊死
        uint32_t beat = beat_ms_.load();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (beat != 0 && (now - beat) > kTimeoutMs) {
            ESP_LOGE(TAG, "poller heartbeat lost for %u ms (HTTP wedged?), restarting system",
                     (unsigned)(now - beat));
            vTaskDelay(pdMS_TO_TICKS(300));   // 让日志写完
            esp_restart();
        }
    }
}

void Poller::Loop() {
    const uint32_t my_gen = generation_.load();   // Stop 后若被快速 Start,老任务凭此严格退出
    auto now_min = []() { return (uint32_t)(esp_timer_get_time() / 1000000ULL / 60); };
    uint32_t last_weather = 0, last_glm = 0, last_round = 0, s_last_mail = 0, s_last_news = 0;
    bool first_round = true;

    // 等 Wi-Fi 获得 IP(否则首个 HTTP 在无网络时发起会永久阻塞——
    // 系统默认时间已 ≥2025,旧的 SNTP 等待被跳过导致 5 秒就发请求)。
    // 最多等 90 秒;之后再等 SNTP(HTTPS 证书需要)最多 60 秒。
    for (int i = 0; i < 180; i++) {
        Beat();
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = {};
        if (netif != nullptr &&
            esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    for (int i = 0; i < 120; i++) {
        Beat();
        time_t t = time(NULL);
        struct tm tm_now;
        localtime_r(&t, &tm_now);
        if (tm_now.tm_year >= 2026 - 1900) break;   // 真实时间(SNTP 后)
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // 开机冷静期:让开 MQTT/OTA/音频引擎/主页字形渲染的内存高峰。
    // 2026-10-25 复测:上述初始化在 4.2s 已全部完成(poller 4.6s 才启动),
    // 且各数据源均为 hermes LAN 纯 HTTP(~2.5K 瞬时客户端,无 TLS)——
    // 老的固定 10s 把首帧数据拖到 16.5s。现改为:基础 2s 覆盖主页字形
    // 渲染,空闲堆 <15K 时逐秒让路,总上限仍 10s(保留老保护语义)。
    if (esp_timer_get_time() < 60000000ULL) {
        for (int i = 0; i < 2; i++) {
            Beat();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        for (int i = 0; i < 8; i++) {
            uint32_t free_now = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
            if (free_now >= 15360) break;
            ESP_LOGI(TAG, "boot cooldown: heap low (%u B), wait 1s", (unsigned)free_now);
            Beat();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "first round ready: free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }

    for (;;) {
        if (!running_.load() || generation_.load() != my_gen) break;   // Stop:退出并自删(释放栈)
        if (paused_.load()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        uint32_t now = now_min();
        Beat();
        // 邮件 APP 的 OK 手动刷新:置位后本循环在 500ms 内跳出等待,
        // 下一轮邮件段立即执行一次真实拉取。
        const bool force_mail = MailStatus::Get().ConsumeForce();
        ESP_LOGI(TAG, "round start (t=%us, refresh=%d)", (unsigned)(esp_timer_get_time() / 1000000ULL),
                 (int)refresh_now_.load());
        // agenda + 金融:每轮(5 分钟)都刷新
        Beat();
        bool agenda_ok = AgendaService::Get().Poll();   // 内含金融三 tab
        last_round = now;

        bool weather_ok = true, glm_ok = true, ds_ok = true;
        bool due = first_round || (now - last_weather >= (uint32_t)s_weather_interval);
        if (due) {
            Beat();
            weather_ok = WeatherService::Get().Poll();
            last_weather = weather_ok ? now_min() : last_weather;
        }
        due = first_round || (now - last_glm >= (uint32_t)s_glm_interval);
        if (due) {
            Beat();
            glm_ok = GlmService::Get().Poll();
            last_glm = glm_ok ? now_min() : last_glm;
            // DeepSeek 余额:与 GLM 同节奏(同服务器,5 分钟)
            Beat();
            ds_ok = DeepseekService::Get().Poll();
        }
        // 邮件未读:独立 2 分钟节奏(独立服务 8768);手动刷新时立即执行
        if (force_mail || now - s_last_mail >= 2) {
            Beat();
            MailStatus::Get().Poll();
            s_last_mail = now_min();
            HomeScreen::Get().SetMailUnread(MailStatus::Get().Unread());
            // 新邮件提醒:通知条(发件人/主题) + 提示音
            char mfrom[64], msubj[128];
            if (MailStatus::Get().TakeNewMail(mfrom, sizeof(mfrom), msubj, sizeof(msubj))) {
                auto* display = Board::GetInstance().GetDisplay();
                if (display != nullptr) {
                    // 发件人取名字部分(< 前)
                    char* lt = strchr(mfrom, '<');
                    if (lt != nullptr) *lt = '\0';
                    // 去尾部空格
                    int fl = strlen(mfrom);
                    while (fl > 0 && mfrom[fl-1] == ' ') mfrom[--fl] = 0;
                    static char notify_buf[128];
                    snprintf(notify_buf, sizeof(notify_buf), "新邮件 %s: %.40s",
                             mfrom[0] ? mfrom : "未知", msubj);
                    display->ShowNotification(notify_buf);
                }
                // 提示音:esp_timer 300ms 后由 main 任务直播。不再建
                // 独立任务(8KB 栈在碎片化后会静默建失败,且任务模式是
                // 已淘汰的老路径);延时错开通知条字形渲染与 codec
                // 重初始化的并发 cache 停顿(见 tetris 结束音教训)。
                static esp_timer_handle_t s_mail_snd = nullptr;
                if (s_mail_snd == nullptr) {
                    const esp_timer_create_args_t targs = {
                        .callback = [](void*) {
                            Application::GetInstance().Schedule([]() {
                                Application::GetInstance().GetAudioService().PlaySound(
                                    Lang::Sounds::OGG_POPUP);
                            });
                        },
                        .name = "mail_snd"};
                    esp_timer_create(&targs, &s_mail_snd);
                }
                if (s_mail_snd != nullptr) {
                    esp_timer_stop(s_mail_snd);   // 防重入:连续新邮件只响一声
                    esp_timer_start_once(s_mail_snd, 300000);
                }
            }
        }
        // AI 新闻:30 分钟节奏(内容一天一更,源=Pi cron 文件);手动刷新时立即执行
        const bool force_news = NewsService::Get().ConsumeForce();
        if (force_news || first_round || now - s_last_news >= (uint32_t)s_news_interval) {
            Beat();
            NewsService::Get().Poll();
            s_last_news = now_min();
        }
        first_round = false;
        last_weather = last_weather;  // no-op,保持变量活跃

        // 全部成功等 5 分钟;有失败 30 秒后重试失败项
        bool round_ok = agenda_ok && weather_ok && glm_ok && ds_ok;
        int wait_seconds = round_ok ? 300 : 30;
        for (int i = 0; i < wait_seconds * 2; i++) {
            if (!running_.load() || generation_.load() != my_gen) break;   // Stop 快速退出
            if (refresh_now_.exchange(false)) break;
            if (MailStatus::Get().ForcePending()) break;   // 邮件手动刷新快速响应
            if (NewsService::Get().ForcePending()) break;  // 新闻手动刷新快速响应
            Beat();   // 空闲等待也算存活,否则 5 分钟间隔会被看门狗误判
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        (void)last_round;
    }
    vTaskDelete(NULL);   // Stop 后自删(释放 12KB 栈)
}

void Poller::Start() {
    if (running_.load()) return;
    // 把配置传导给各服务实例(否则其 URL/key 为空,请求全部无效)
    WeatherService::Get().Start(s_weather_url, s_weather_interval);
    GlmService::Get().Start(s_glm_key.c_str(), s_glm_interval);
    AgendaService::Get().Start(s_agenda_url, s_agenda_interval);
    MailStatus::Get().Start(s_mail_url, s_mail_interval);
    NewsService::Get().Start(s_news_url, s_news_interval);
    running_.store(true);
    // 5KB 栈:实测峰值 2.8KB(全数据源局域网纯 HTTP,cJSON 解析),
    // 栈小一号也更容易在内存碎片化后重建(见下方失败重启)。
    // 仲裁会以 ~1Hz 重试 Start;连续 ~10s 建不出任务说明内存已碎到
    // 无法服务(看门狗对"想跑跑不起来"是盲区),整机重启自愈。
    static int s_start_failures = 0;
    if (xTaskCreate([](void* arg) { static_cast<Poller*>(arg)->Loop(); },
                    "poller", 5120, this, 3, nullptr) != pdPASS) {
        running_.store(false);
        if (++s_start_failures >= 10) {
            ESP_LOGE(TAG, "poller task create failed x%d, heap too fragmented, restarting system",
                     s_start_failures);
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        ESP_LOGE(TAG, "poller task create failed (%d/10)", s_start_failures);
        return;
    }
    s_start_failures = 0;
    // 看门狗只建一次(Stop/Start 重建 poller 时复用同一监控任务)
    // 实测峰值仅 ~300B,1KB 栈足够
    static bool s_watchdog_started = false;
    if (!s_watchdog_started) {
        if (xTaskCreate([](void* arg) { static_cast<Poller*>(arg)->WatchdogLoop(); },
                        "poller_wd", 1024, this, 2, nullptr) == pdPASS) {
            s_watchdog_started = true;
            ESP_LOGI(TAG, "watchdog started (150s heartbeat timeout)");
        } else {
            ESP_LOGE(TAG, "poller watchdog create failed");
        }
    }
}
