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
    bool good = false;
    if (cJSON_IsTrue(okj) && cJSON_IsNumber(pctj)) {
        int pct = pctj->valueint;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        remaining_pct_.store(pct);
        ESP_LOGI(TAG, "GLM 5h 剩余 %d%%", pct);
        good = true;
    }
    cJSON_Delete(root);
    return good;
}