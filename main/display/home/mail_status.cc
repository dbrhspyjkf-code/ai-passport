// mail_status.cc —— 未读邮件查询实现(纯 HTTP,失败保留旧值)。
#include "mail_status.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "poller_http.h"

static const char* TAG = "mail";

MailStatus& MailStatus::Get() {
    static MailStatus instance;
    return instance;
}

void MailStatus::Start(const std::string& base_url, int interval_minutes) {
    if (base_url.empty()) return;
    base_url_ = base_url + "/mail/status";
    interval_minutes_ = interval_minutes > 0 ? interval_minutes : 2;
}

bool MailStatus::GetMessage(int idx, char* from, size_t from_cap,
                            char* subject, size_t subject_cap,
                            char* preview, size_t preview_cap) const {
    if (idx < 0 || idx >= msg_count_) return false;
    snprintf(from, from_cap, "%s", msgs_[idx].from);
    snprintf(subject, subject_cap, "%s", msgs_[idx].subject);
    snprintf(preview, preview_cap, "%s", msgs_[idx].preview);
    return true;
}

bool MailStatus::TakeNewMail(char* from, size_t from_cap, char* subject, size_t subject_cap) {
    if (!has_new_) return false;
    has_new_ = false;
    snprintf(from, from_cap, "%s", new_from_);
    snprintf(subject, subject_cap, "%s", new_subject_);
    return true;
}

void MailStatus::Poll() {
    ESP_LOGI(TAG, "mail poll (url=%s)", base_url_.empty() ? "(空)" : base_url_.c_str());
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
        } else {
            ESP_LOGW(TAG, "open 失败: %s", esp_err_to_name(ESP_FAIL));
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (!ok) {
        ESP_LOGW(TAG, "邮件状态查询失败(len=%d),保留旧值", len);
        return;
    }
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return;
    cJSON* unread = cJSON_GetObjectItem(root, "unread");
    if (cJSON_IsNumber(unread)) {
        int prev = unread_.load();
        int now = unread->valueint;
        unread_.store(now);
        // 新邮件到达(排除首次同步 -1→N)
        if (prev >= 0 && now > prev) {
            snprintf(new_from_, sizeof(new_from_), "%s", "");
            snprintf(new_subject_, sizeof(new_subject_), "%s", "");
            cJSON* from = cJSON_GetObjectItem(root, "from");
            cJSON* subj = cJSON_GetObjectItem(root, "subject");
            if (cJSON_IsString(from) && from->valuestring)
                snprintf(new_from_, sizeof(new_from_), "%s", from->valuestring);
            if (cJSON_IsString(subj) && subj->valuestring)
                snprintf(new_subject_, sizeof(new_subject_), "%s", subj->valuestring);
            has_new_ = true;
            ESP_LOGI(TAG, "新邮件! from=%s subj=%.32s", new_from_, new_subject_);
        }
        // 未读列表(Email APP 数据)
        msg_count_ = 0;
        cJSON* msgs = cJSON_GetObjectItem(root, "messages");
        if (cJSON_IsArray(msgs)) {
            int n = cJSON_GetArraySize(msgs);
            if (n > 10) n = 10;
            for (int i = 0; i < n; i++) {
                cJSON* m = cJSON_GetArrayItem(msgs, i);
                cJSON* mf = cJSON_GetObjectItem(m, "from");
                cJSON* ms = cJSON_GetObjectItem(m, "subject");
                cJSON* mp = cJSON_GetObjectItem(m, "preview");
                snprintf(msgs_[i].from, sizeof(msgs_[i].from), "%s",
                         cJSON_IsString(mf) && mf->valuestring ? mf->valuestring : "");
                snprintf(msgs_[i].subject, sizeof(msgs_[i].subject), "%s",
                         cJSON_IsString(ms) && ms->valuestring ? ms->valuestring : "");
                snprintf(msgs_[i].preview, sizeof(msgs_[i].preview), "%s",
                         cJSON_IsString(mp) && mp->valuestring ? mp->valuestring : "");
            }
            msg_count_ = n;
        }
        ESP_LOGI(TAG, "未读邮件 %d", unread->valueint);
    }
    version_.fetch_add(1);   // 数据已更新,界面可据此重绘
    cJSON_Delete(root);
}
