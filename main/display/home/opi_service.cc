// opi_service.cc —— OrangePi 系统状态查询实现(纯 HTTP,失败保留旧值)。
#include "opi_service.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "poller_http.h"

static const char* TAG = "opi";

OpiService& OpiService::Get() {
    static OpiService instance;
    return instance;
}

void OpiService::Start(const std::string& base_url) {
    if (base_url.empty()) return;
    base_url_ = base_url + "/system/status";
}

static void copy_str(char* dst, size_t cap, cJSON* parent, const char* key) {
    cJSON* j = cJSON_GetObjectItem(parent, key);
    snprintf(dst, cap, "%s", cJSON_IsString(j) && j->valuestring ? j->valuestring : "");
}

void OpiService::Poll() {
    ESP_LOGI(TAG, "opi poll (url=%s)", base_url_.empty() ? "(空)" : base_url_.c_str());
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
        ESP_LOGW(TAG, "OrangePi 状态查询失败(len=%d),保留旧值", len);
        return;
    }
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return;
    cJSON* jok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(jok)) {
        ESP_LOGW(TAG, "OrangePi 状态无数据");
        cJSON_Delete(root);
        return;
    }
    Data d;
    cJSON* host = cJSON_GetObjectItem(root, "host");
    if (cJSON_IsObject(host)) {
        copy_str(d.hostname, sizeof(d.hostname), host, "hostname");
        copy_str(d.ip, sizeof(d.ip), host, "ip");
        copy_str(d.kernel, sizeof(d.kernel), host, "kernel");
        copy_str(d.uptime, sizeof(d.uptime), host, "uptime_human");
    }
    cJSON* load = cJSON_GetObjectItem(root, "load");
    if (cJSON_IsObject(load)) {
        cJSON* v;
        if ((v = cJSON_GetObjectItem(load, "load1")) && cJSON_IsNumber(v)) d.load1 = v->valuedouble;
        if ((v = cJSON_GetObjectItem(load, "load5")) && cJSON_IsNumber(v)) d.load5 = v->valuedouble;
        if ((v = cJSON_GetObjectItem(load, "load15")) && cJSON_IsNumber(v)) d.load15 = v->valuedouble;
        if ((v = cJSON_GetObjectItem(load, "cpu_percent")) && cJSON_IsNumber(v)) d.cpu_percent = v->valuedouble;
    }
    cJSON* mem = cJSON_GetObjectItem(root, "memory");
    if (cJSON_IsObject(mem)) {
        cJSON* v;
        if ((v = cJSON_GetObjectItem(mem, "percent")) && cJSON_IsNumber(v)) d.mem_percent = v->valuedouble;
        if ((v = cJSON_GetObjectItem(mem, "used_mb")) && cJSON_IsNumber(v)) d.mem_used_mb = v->valueint;
        if ((v = cJSON_GetObjectItem(mem, "total_mb")) && cJSON_IsNumber(v)) d.mem_total_mb = v->valueint;
        if ((v = cJSON_GetObjectItem(mem, "swap_percent")) && cJSON_IsNumber(v)) d.swap_percent = v->valueint;
    }
    cJSON* disk = cJSON_GetObjectItem(root, "disk");
    if (cJSON_IsArray(disk) && cJSON_GetArraySize(disk) > 0) {
        cJSON* d0 = cJSON_GetArrayItem(disk, 0);
        cJSON* v;
        if ((v = cJSON_GetObjectItem(d0, "percent")) && cJSON_IsNumber(v)) d.disk_percent = v->valuedouble;
        if ((v = cJSON_GetObjectItem(d0, "used_gb")) && cJSON_IsNumber(v)) d.disk_used_gb = v->valuedouble;
        if ((v = cJSON_GetObjectItem(d0, "total_gb")) && cJSON_IsNumber(v)) d.disk_total_gb = v->valuedouble;
    }
    cJSON* therm = cJSON_GetObjectItem(root, "thermal");
    if (cJSON_IsObject(therm)) {
        cJSON* v;
        if ((v = cJSON_GetObjectItem(therm, "cpu_temp_c")) && cJSON_IsNumber(v)) d.cpu_temp_c = v->valuedouble;
        if ((v = cJSON_GetObjectItem(therm, "fan_rpm")) && cJSON_IsNumber(v)) d.fan_rpm = v->valueint;
    }
    d.valid = true;
    data_ = d;
    version_.fetch_add(1);
    ESP_LOGI(TAG, "opi: %s load=%.2f/%.2f/%.2f cpu=%.1f mem=%d/%dMB(%.1f%%) swap=%d "
             "disk=%.1f%%(%.0fG/%.0fG) temp=%.0f fan=%d",
             d.hostname, d.load1, d.load5, d.load15, d.cpu_percent,
             d.mem_used_mb, d.mem_total_mb, d.mem_percent, d.swap_percent,
             d.disk_percent, d.disk_used_gb, d.disk_total_gb, d.cpu_temp_c, d.fan_rpm);
    cJSON_Delete(root);
}
