// glm_service.cc —— 智谱 GLM 用量查询实现(移植自 M5-DIAL glm_client,MIT)。
#include "glm_service.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "glm";

GlmService& GlmService::Get() {
    static GlmService instance;
    return instance;
}

void GlmService::Start(const char* api_key, int interval_minutes) {
    if (api_key == nullptr || api_key[0] == '\0') return;
    api_key_ = api_key;
    interval_minutes_ = interval_minutes > 0 ? interval_minutes : 5;
}

bool GlmService::Poll() {
    // hermes 缓存端点(服务端后台 5 分钟刷新,设备毫秒级读取)
    static const char* kUrl = "http://192.168.1.200:8766/glm/quota";
    static char body[512];
    int len = 0;
    esp_http_client_config_t cfg = {};
    cfg.url = kUrl;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    bool ok = false;
    if (client != nullptr) {
        if (esp_http_client_open(client, 0) == ESP_OK) {
            esp_http_client_fetch_headers(client);
            int n;
            while ((n = esp_http_client_read(client, body + len, sizeof(body) - len - 1)) > 0) {
                len += n;
                if (len >= (int)sizeof(body) - 1) break;
            }
            body[len] = '\0';
            if (esp_http_client_get_status_code(client) == 200 && len > 0) ok = true;
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (!ok) {
        ESP_LOGW(TAG, "GLM 用量拉取失败(len=%d)", len);
        return false;
    }
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return false;
    cJSON* okj = cJSON_GetObjectItem(root, "ok");
    cJSON* pctj = cJSON_GetObjectItem(root, "remaining_pct");
    cJSON* pct7 = cJSON_GetObjectItem(root, "pct_7d");
    cJSON* rst = cJSON_GetObjectItem(root, "reset_5h_ms");
    bool good = false;
    if (cJSON_IsTrue(okj) && cJSON_IsNumber(pctj)) {
        remaining_pct_.store(pctj->valueint);
        if (cJSON_IsNumber(pct7)) pct_7d_.store(pct7->valueint);
        if (cJSON_IsNumber(rst)) {
            reset_ms_.store((int64_t)rst->valuedouble);
            // 锚定 (reset - 设备time()) 差值 + 单调钟。
            // (ota.cc 曾把 TZ offset 加进 epoch 导致系统钟快 8h,已修;
            //  现在 time() 是干净 UTC,直接相减即可)
            anchor_reset_minus_now_ms_ = reset_ms_.load()
                - (int64_t)time(NULL) * 1000;
            anchor_mono_ms_ = esp_timer_get_time() / 1000;
        }
        good = true;
        ESP_LOGI(TAG, "GLM 5h=%d%% 7d=%d%% reset=%lld",
                 pctj->valueint,
                 pct_7d_.load(),
                 (long long)reset_ms_.load());
    }
    cJSON_Delete(root);
    return good;
}
void GlmService::FormatReset(char* buf, int cap) {
    int64_t reset = Get().ResetMs();
    if (reset <= 0) { buf[0] = '\0'; return; }
    // Tab5 同语义: 显示重置时刻的本地时钟时间(HH:MM),非倒计时。
    // (设备钟曾被 ota.cc 污染为本地伪 epoch;现在修好了,reset 是 UTC,
    //  localtime 需要 TZ=CST 才能转出北京时刻。)
    static bool tz_set = false;
    if (!tz_set) { setenv("TZ", "CST-8", 1); tzset(); tz_set = true; }
    time_t t = (time_t)(reset / 1000);
    struct tm tv;
    localtime_r(&t, &tv);
    snprintf(buf, cap, "%02d:%02d", tv.tm_hour, tv.tm_min);
}
