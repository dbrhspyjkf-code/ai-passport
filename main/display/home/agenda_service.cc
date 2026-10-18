// agenda_service.cc —— hermes /agenda 拉取实现(一次性连接,失败保留旧值)。
#include "agenda_service.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "finance_app.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "agenda";

AgendaService& AgendaService::Get() {
    static AgendaService instance;
    return instance;
}

void AgendaService::RequestRefresh() { refresh_now_.store(true); }

void AgendaService::Start(const std::string& base_url, int interval_minutes) {
    if (base_url.empty()) return;
    url_ = base_url + "/agenda";
    interval_minutes_ = interval_minutes > 0 ? interval_minutes : 5;
}

std::string AgendaService::Text() const {
    // 两个 text 只在后台 task 写;UI 读走拷贝,容忍竞态读到的中间态
    if (second_text_.empty()) return next_text_;
    return next_text_ + "\n" + second_text_;
}

bool AgendaService::Poll() {
    char body[1024];
    bool agenda_ok = false;
    {
        esp_http_client_config_t cfg = {};
        cfg.url = url_.c_str();
        cfg.method = HTTP_METHOD_GET;
        cfg.timeout_ms = 8000;
        cfg.buffer_size = 2048;
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        bool ok = false;
        bool has_next = false;
        bool has_second = false;
        char text[128] = {0};
        char second[128] = {0};
        int len = 0;
        if (client == nullptr) {
            ESP_LOGW(TAG, "http_client init 失败");
        }
        if (client != nullptr) {
            esp_err_t open_err = esp_http_client_open(client, 0);
            if (open_err == ESP_OK) {
                esp_http_client_fetch_headers(client);
                int n;
                while ((n = esp_http_client_read(client, body + len, sizeof(body) - len - 1)) > 0) {
                    len += n;
                    if (len >= (int)sizeof(body) - 1) break;
                }
                body[len] = '\0';
                if (esp_http_client_get_status_code(client) == 200 && len > 0) ok = true;
            } else {
                ESP_LOGW(TAG, "open 失败: %s", esp_err_to_name(open_err));
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
        if (ok) {
            cJSON* root = cJSON_Parse(body);
            if (root != nullptr) {
                cJSON* okj = cJSON_GetObjectItem(root, "ok");
                cJSON* next = cJSON_GetObjectItem(root, "next");
                if (cJSON_IsTrue(okj)) {
                    has_next = true;
                    if (cJSON_IsObject(next)) {
                        cJSON* t = cJSON_GetObjectItem(next, "text");
                        if (cJSON_IsString(t) && t->valuestring[0] != '\0') {
                            strncpy(text, t->valuestring, sizeof(text) - 1);
                        } else {
                            has_next = false;
                        }
                    } else {
                        has_next = false;
                    }
                    cJSON* sec = cJSON_GetObjectItem(root, "second");
                    if (has_next && cJSON_IsObject(sec)) {
                        cJSON* st = cJSON_GetObjectItem(sec, "text");
                        if (cJSON_IsString(st) && st->valuestring[0] != '\0') {
                            strncpy(second, st->valuestring, sizeof(second) - 1);
                            has_second = true;
                        }
                    }
                }
                cJSON_Delete(root);
            }
        } else {
            ESP_LOGW(TAG, "agenda 拉取失败(len=%d),保留旧值", len);
        }
        if (ok) {
            next_text_ = has_next ? std::string(text) : std::string();
            second_text_ = (has_next && has_second) ? std::string(second) : std::string();
            ESP_LOGI(TAG, "agenda: %s%s%s", has_next ? text : "(无安排)",
                     has_second ? " | " : "", has_second ? second : "");
            agenda_ok = true;
        }
    }

    // 金融三 tab 同源(hermes):无论 agenda 成败都拉取
    for (int tab = 0; tab < 3; tab++) {
        FinanceApp::Get().RefreshTab(tab);
    }
    return agenda_ok;
}
