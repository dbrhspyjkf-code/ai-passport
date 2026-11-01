// health_service.cc —— 健康数据查询实现(纯 HTTP,失败保留旧值)。
#include "health_service.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "poller_http.h"

static const char* TAG = "health";

HealthService& HealthService::Get() {
    static HealthService instance;
    return instance;
}

void HealthService::Start(const std::string& base_url) {
    if (base_url.empty()) return;
    base_url_ = base_url + "/api/health";
}

void HealthService::Poll() {
    ESP_LOGI(TAG, "health poll (url=%s)", base_url_.empty() ? "(空)" : base_url_.c_str());
    if (base_url_.empty()) return;
    char* body = poller_http::body;   // 仅 poller 任务调用,串行安全
    const size_t kBodyCap = sizeof(poller_http::body);
    int len = 0;
    esp_http_client_config_t cfg = {};
    cfg.url = base_url_.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    bool ok = false;
    if (client != nullptr) {
        if (esp_http_client_open(client, 0) == ESP_OK) {
            esp_http_client_fetch_headers(client);
            int n;
            while ((n = esp_http_client_read(client, body + len, kBodyCap - len - 1)) > 0) {
                len += n;
                if (len >= (int)kBodyCap - 1) break;
            }
            body[len] = '\0';
            if (esp_http_client_get_status_code(client) == 200 && len > 0) ok = true;
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (!ok) {
        ESP_LOGW(TAG, "健康数据查询失败(len=%d),保留旧值", len);
        return;
    }
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return;
    cJSON* jok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(jok)) {
        ESP_LOGW(TAG, "健康数据无数据");
        cJSON_Delete(root);
        return;
    }
    // 更新时刻:"2026-10-31T20:03:02+08:00" → "20:03"
    updated_at_[0] = '\0';
    cJSON* ju = cJSON_GetObjectItem(root, "updated_at");
    if (cJSON_IsString(ju) && ju->valuestring) {
        const char* t = strchr(ju->valuestring, 'T');
        if (t != nullptr && strlen(t) >= 6) {
            snprintf(updated_at_, sizeof(updated_at_), "%.5s", t + 1);
        }
    }
    cJSON* types = cJSON_GetObjectItem(root, "types");
    if (cJSON_IsObject(types)) {
        for (int i = 0; i < kMetricCount; i++) {
            metrics_[i].present = false;
            cJSON* m = cJSON_GetObjectItem(types, metrics_[i].type);
            cJSON* v = m != nullptr ? cJSON_GetObjectItem(m, "value") : nullptr;
            if (cJSON_IsNumber(v)) {
                metrics_[i].value = (float)v->valuedouble;
                metrics_[i].present = true;
            }
        }
    }
    valid_ = true;
    version_.fetch_add(1);
    ESP_LOGI(TAG, "健康数据 %d/%d 项 (%s)",
             (int)(metrics_[0].present), kMetricCount, updated_at_);
    cJSON_Delete(root);
}
