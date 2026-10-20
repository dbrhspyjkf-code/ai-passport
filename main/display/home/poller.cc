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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "agenda_service.h"
#include "deepseek_service.h"
#include "finance_app.h"
#include "glm_service.h"
#include "home_screen.h"
#include "mail_status.h"
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

void PollerConfig(const std::string& weather_url, int weather_min,
                  const char* glm_key, int glm_min,
                  const std::string& agenda_url, int agenda_min,
                  const std::string& mail_url, int mail_min) {
    s_weather_url = weather_url;
    s_weather_interval = weather_min;
    s_glm_key = glm_key;
    s_glm_interval = glm_min;
    s_agenda_url = agenda_url;
    s_agenda_interval = agenda_min;
    s_mail_url = mail_url;
    s_mail_interval = mail_min;
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
    uint32_t last_weather = 0, last_glm = 0, last_round = 0, s_last_mail = 0;
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

    for (;;) {
        if (!running_.load() || generation_.load() != my_gen) break;   // Stop:退出并自删(释放栈)
        if (paused_.load()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        uint32_t now = now_min();
        Beat();
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
        // 邮件未读:独立 2 分钟节奏(独立服务 8768)
        if (now - s_last_mail >= 2) {
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
                // 提示音(独立任务推官方管线,已验证模式)
                static bool s_snd_busy = false;
                if (!s_snd_busy) {
                    s_snd_busy = true;
                    xTaskCreate(
                        [](void*) {
                            Application::GetInstance().GetAudioService().PlaySound(
                                Lang::Sounds::OGG_POPUP);
                            vTaskDelay(pdMS_TO_TICKS(800));
                            s_snd_busy = false;
                            vTaskDelete(nullptr);
                        },
                        "mail_snd", 8192, nullptr, 3, nullptr);
                }
            }
        }
        first_round = false;
        last_weather = last_weather;  // no-op,保持变量活跃

        // 全部成功等 5 分钟;有失败 30 秒后重试失败项
        bool round_ok = agenda_ok && weather_ok && glm_ok && ds_ok;
        int wait_seconds = round_ok ? 300 : 30;
        for (int i = 0; i < wait_seconds * 2; i++) {
            if (!running_.load() || generation_.load() != my_gen) break;   // Stop 快速退出
            if (refresh_now_.exchange(false)) break;
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
    running_.store(true);
    // 8KB 栈:全部数据源均为局域网纯 HTTP(TLS 早已移到服务端),峰值是
    // cJSON 解析;栈小一号也更容易在内存碎片化后重建(见下方失败重启)。
    // 仲裁会以 ~1Hz 重试 Start;连续 ~10s 建不出任务说明内存已碎到
    // 无法服务(看门狗对"想跑跑不起来"是盲区),整机重启自愈。
    static int s_start_failures = 0;
    if (xTaskCreate([](void* arg) { static_cast<Poller*>(arg)->Loop(); },
                    "poller", 8192, this, 3, nullptr) != pdPASS) {
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
    static bool s_watchdog_started = false;
    if (!s_watchdog_started) {
        if (xTaskCreate([](void* arg) { static_cast<Poller*>(arg)->WatchdogLoop(); },
                        "poller_wd", 2048, this, 2, nullptr) == pdPASS) {
            s_watchdog_started = true;
            ESP_LOGI(TAG, "watchdog started (150s heartbeat timeout)");
        } else {
            ESP_LOGE(TAG, "poller watchdog create failed");
        }
    }
}
