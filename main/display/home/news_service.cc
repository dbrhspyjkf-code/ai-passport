// news_service.cc —— AI 新闻查询实现(纯 HTTP,失败保留旧值)。
#include "news_service.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "poller_http.h"

static const char* TAG = "news";

NewsService& NewsService::Get() {
    static NewsService instance;
    return instance;
}

void NewsService::Start(const std::string& base_url, int interval_minutes) {
    if (base_url.empty()) return;
    base_url_ = base_url + "/news/ai";
    interval_minutes_ = interval_minutes > 0 ? interval_minutes : 30;
}

bool NewsService::GetItem(int idx, char* label, size_t label_cap,
                          char* text, size_t text_cap) const {
    if (idx < 0 || idx >= total_) return false;
    int seen = 0;
    for (int s = 0; s < sec_count_; s++) {
        if (idx < seen + secs_[s].count) {
            snprintf(label, label_cap, "%s", secs_[s].label);
            snprintf(text, text_cap, "%s", secs_[s].items[idx - seen]);
            return true;
        }
        seen += secs_[s].count;
    }
    return false;
}

void NewsService::Poll() {
    ESP_LOGI(TAG, "news poll (url=%s)", base_url_.empty() ? "(空)" : base_url_.c_str());
    if (base_url_.empty()) return;
    char* body = poller_http::body;   // 单 poller 任务串行复用(见 poller_http.h)
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
        ESP_LOGW(TAG, "AI 新闻查询失败(len=%d),保留旧值", len);
        return;
    }
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return;
    cJSON* jok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(jok)) {
        ESP_LOGW(TAG, "AI 新闻无数据");
        cJSON_Delete(root);
        return;
    }
    cJSON* jdate = cJSON_GetObjectItem(root, "date");
    snprintf(date_, sizeof(date_), "%s",
             cJSON_IsString(jdate) && jdate->valuestring ? jdate->valuestring : "");
    cJSON* jsum = cJSON_GetObjectItem(root, "summary");
    snprintf(summary_, sizeof(summary_), "%s",
             cJSON_IsString(jsum) && jsum->valuestring ? jsum->valuestring : "");
    sec_count_ = 0;
    total_ = 0;
    cJSON* jsecs = cJSON_GetObjectItem(root, "secs");
    if (cJSON_IsArray(jsecs)) {
        int n = cJSON_GetArraySize(jsecs);
        if (n > kMaxSecs) n = kMaxSecs;
        for (int s = 0; s < n; s++) {
            cJSON* js = cJSON_GetArrayItem(jsecs, s);
            cJSON* jl = cJSON_GetObjectItem(js, "label");
            snprintf(secs_[s].label, sizeof(secs_[s].label), "%s",
                     cJSON_IsString(jl) && jl->valuestring ? jl->valuestring : "新闻");
            secs_[s].count = 0;
            cJSON* jitems = cJSON_GetObjectItem(js, "items");
            if (cJSON_IsArray(jitems)) {
                int m = cJSON_GetArraySize(jitems);
                if (m > kMaxItemsPerSec) m = kMaxItemsPerSec;
                for (int i = 0; i < m; i++) {
                    cJSON* ji = cJSON_GetArrayItem(jitems, i);
                    if (cJSON_IsString(ji) && ji->valuestring) {
                        snprintf(secs_[s].items[i], sizeof(secs_[s].items[i]),
                                 "%s", ji->valuestring);
                    }
                }
                secs_[s].count = m;
                total_ += m;
            }
            if (secs_[s].count > 0) sec_count_ = s + 1;
        }
    }
    valid_ = true;
    version_.fetch_add(1);   // 数据已更新,界面可据此重绘
    ESP_LOGI(TAG, "AI 新闻 %d 分区 %d 条 (%s)", sec_count_, total_, date_);
    cJSON_Delete(root);
}
