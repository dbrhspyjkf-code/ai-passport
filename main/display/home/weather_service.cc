// weather_service.cc —— Open-Meteo 拉取实现。一次性 esp_http_client + cJSON。
#include "weather_service.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "esp_crt_bundle.h"

#include "poller_http.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "weather";

static const char* kWindDirs[8] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};

const char* WeatherWindDirCn(int deg) {
    // 22.5° 一档,deg 归一化到 [0,360)
    int d = ((deg % 360) + 360) % 360;
    int idx = (int)((d + 22) / 45) % 8;
    return kWindDirs[idx];
}

int WeatherWindScale(int kmh) {
    // 蒲福风级近似界限(km/h): 1,6,12,20,29,39,50,62,75,89,103,118
    static const int bounds[] = {1, 6, 12, 20, 29, 39, 50, 62, 75, 89, 103, 118};
    for (int i = 0; i < 12; i++) {
        if (kmh < bounds[i]) return i;
    }
    return 12;
}

const char* WeatherCodeCn(int code) {
    switch (code) {
        case 0: return "晴";
        case 1: case 2: return "多云";
        case 3: return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: case 56: case 57: return "毛毛雨";
        case 61: case 63: case 65: case 66: case 67: case 80: case 81: case 82: return "雨";
        case 71: case 73: case 75: case 77: case 85: case 86: return "雪";
        case 95: case 96: case 99: return "雷雨";
        default: return "—";
    }
}

WeatherService& WeatherService::Get() {
    static WeatherService instance;
    return instance;
}

void WeatherService::Start(const std::string& url, int interval_minutes) {
    url_ = url;
    interval_minutes_ = interval_minutes > 0 ? interval_minutes : 10;
}

bool WeatherService::Poll() {
    char* body = poller_http::body;   // 仅 poller 任务调用,串行安全
    const size_t kBodyCap = sizeof(poller_http::body);
    int len = 0;
    esp_http_client_config_t cfg = {};
    cfg.url = url_.c_str();
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
        ESP_LOGW(TAG, "天气拉取失败,保留旧数据");
        return false;
    }
    WeatherSnapshot snap = {};
    cJSON* root = cJSON_Parse(body);
    if (root != nullptr) {
        cJSON* okj = cJSON_GetObjectItem(root, "ok");
        if (cJSON_IsTrue(okj)) {
            auto str_num = [&](const char* k, int* out) {
                cJSON* v = cJSON_GetObjectItem(root, k);
                if (cJSON_IsString(v) && v->valuestring) *out = atoi(v->valuestring);
                else if (cJSON_IsNumber(v)) *out = (int)v->valuedouble;
            };
            str_num("temp_c", &snap.temperature_c);
            str_num("humidity", &snap.humidity_pct);
            cJSON* cond = cJSON_GetObjectItem(root, "condition");
            if (cJSON_IsString(cond) && cond->valuestring) {
                strncpy(snap.condition_cn, cond->valuestring, sizeof(snap.condition_cn) - 1);
            }
            cJSON* city = cJSON_GetObjectItem(root, "city");
            if (cJSON_IsString(city) && city->valuestring) {
                strncpy(snap.city, city->valuestring, sizeof(snap.city) - 1);
            }
            snap.valid = snap.condition_cn[0] != '\0';
        }
        cJSON_Delete(root);
    }
    if (!snap.valid) {
        ESP_LOGW(TAG, "天气响应解析失败,保留旧数据");
        return false;
    }
    snap.updated_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    snapshot_ = snap;
    ESP_LOGI(TAG, "天气更新: %d° %s 湿度%d%% (%s)",
             snap.temperature_c, snap.condition_cn, snap.humidity_pct, snap.city);
    return true;
}