// ipo_service.cc —— 新股申购查询实现。
#include "ipo_service.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "poller_http.h"

static const char* TAG = "ipo";

IpoService& IpoService::Get() {
    static IpoService instance;
    return instance;
}

void IpoService::Start(const std::string& base_url) {
    if (base_url.empty()) return;
    base_url_ = base_url + "/stocks/ipo";
}

void IpoService::Poll() {
    if (base_url_.empty()) return;
    char* body = poller_http::body;
    const size_t kCap = sizeof(poller_http::body);
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
            if (esp_http_client_get_status_code(client) == 200) {
                int n;
                while ((n = esp_http_client_read(client, body + len, kCap - len - 1)) > 0) {
                    len += n;
                    if (len >= (int)kCap - 1) break;
                }
                body[len] = '\0';
                ok = true;
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (!ok) return;

    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return;
    text_[0] = '\0';
    cJSON* items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) {
        cJSON* first = cJSON_GetArrayItem(items, 0);
        if (first != nullptr) {
            cJSON* jn = cJSON_GetObjectItem(first, "name");
            cJSON* jd = cJSON_GetObjectItem(first, "date");
            cJSON* jt = cJSON_GetObjectItem(first, "today");
            const char* name = (cJSON_IsString(jn) && jn->valuestring) ? jn->valuestring : "";
            const char* date = (cJSON_IsString(jd) && jd->valuestring) ? jd->valuestring : "";
            bool today = cJSON_IsTrue(jt);
            if (name[0] != '\0') {
                snprintf(text_, sizeof(text_), "新股 %s %s",
                         name, today ? "今日" : date);
            }
        }
    }
    cJSON_Delete(root);
    valid_ = true;
    version_.fetch_add(1);
    ESP_LOGI(TAG, "IPO: '%s'", text_);
}
