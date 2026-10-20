// deepseek_service.cc —— DeepSeek 余额拉取实现(模式同 glm_service)。
#include "deepseek_service.h"

#include <cstdio>
#include <cstdlib>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "deepseek";

DeepseekService& DeepseekService::Get() {
    static DeepseekService instance;
    return instance;
}

bool DeepseekService::Poll() {
    // hermes 端点(服务端实时查 DeepSeek /user/balance,设备毫秒级读取)
    static const char* kUrl = "http://192.168.1.200:8766/api/deepseek/balance";
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
        ESP_LOGW(TAG, "DeepSeek 余额拉取失败(len=%d)", len);
        return false;
    }
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return false;
    cJSON* okj = cJSON_GetObjectItem(root, "ok");
    cJSON* balj = cJSON_GetObjectItem(root, "total_balance");
    bool good = false;
    if (cJSON_IsTrue(okj) && cJSON_IsString(balj) && balj->valuestring != nullptr) {
        double bal = atof(balj->valuestring);
        if (bal < 0) bal = 0;
        balance_cents_.store((int)(bal * 100 + 0.5));
        ESP_LOGI(TAG, "DeepSeek 余额 %.2f 元", bal);
        good = true;
    }
    cJSON_Delete(root);
    return good;
}
