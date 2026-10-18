// pod_app.cc —— Pod 控制实现(Home Assistant REST,无专辑图精简版)。
#include "pod_app.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if __has_include("pod_secrets.h")
#include "pod_secrets.h"
#else
#include "sonos_secrets.example.h"
#endif

static const char* TAG = "pod";

#define C_BG 0x0a1f14
#define C_FG 0xb7f5c4
#define C_DIM 0x4f7a5e
#define C_HINT 0x34573f

LV_FONT_DECLARE(font_home_cn_16);

// UTC 时间戳解析(M5-DIAL 同款验证实现,绕开 newlib mktime 时区问题)
static bool parse_ha_utc_timestamp(const char* value, time_t* out) {
    int y, mo, d, h, mi, se;
    if (sscanf(value, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return false;
    y -= mo <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t seconds = era * 146097 + doe - 719468;
    seconds = seconds * 86400 + h * 3600 + mi * 60 + se;
    // 时区后缀 +HH:MM / -HH:MM
    const char* tz = value + 19;
    if (*tz == '+' || *tz == '-') {
        int th, tm;
        if (sscanf(tz + 1, "%d:%d", &th, &tm) == 2) {
            const int off = th * 3600 + tm * 60;
            seconds += *tz == '+' ? -off : off;
        }
    }
    *out = (time_t)seconds;
    return true;
}

PodApp& PodApp::Get() {
    static PodApp instance;
    return instance;
}

// ---------------- HTTP 辅助(sonos task 专用) ----------------

static bool ha_get(char* body, size_t cap, const char* path) {
    static char url[160];
    snprintf(url, sizeof(url), "%s%s", POD_HA_BASE_URL, path);
    int len = 0;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 6000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return false;
    esp_http_client_set_header(client, "Authorization", "Bearer " POD_HA_TOKEN);
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int n;
        while ((n = esp_http_client_read(client, body + len, (int)cap - len - 1)) > 0) {
            len += n;
            if (len >= (int)cap - 1) break;
        }
        body[len] = '\0';
        ok = esp_http_client_get_status_code(client) == 200 && len > 0;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static bool ha_post(const char* service, const char* extra_fields) {
    static char url[176], post_body[192];
    snprintf(url, sizeof(url), "%s/api/services/media_player/%s", POD_HA_BASE_URL, service);
    if (extra_fields && extra_fields[0]) {
        snprintf(post_body, sizeof(post_body),
                 "{\"entity_id\": \"%s\", %s}", POD_ENTITY_ID, extra_fields);
    } else {
        snprintf(post_body, sizeof(post_body), "{\"entity_id\": \"%s\"}", POD_ENTITY_ID);
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 6000;
    cfg.buffer_size = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return false;
    esp_http_client_set_header(client, "Authorization", "Bearer " POD_HA_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_open(client, strlen(post_body));
    esp_http_client_write(client, post_body, strlen(post_body));
    esp_http_client_fetch_headers(client);
    bool ok = esp_http_client_get_status_code(client) >= 200 &&
              esp_http_client_get_status_code(client) < 300;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

// ---------------- 状态获取与命令 ----------------

bool PodApp::FetchStatus() {
    static char body[3072];
    if (!ha_get(body, sizeof(body), "/api/states/" POD_ENTITY_ID)) {
        ESP_LOGW(TAG, "状态查询失败");
        return false;
    }
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return false;
    cJSON* state = cJSON_GetObjectItem(root, "state");
    cJSON* attrs = cJSON_GetObjectItem(root, "attributes");
    if (!cJSON_IsString(state) || !cJSON_IsObject(attrs)) {
        cJSON_Delete(root);
        return false;
    }
    snprintf(state_, sizeof(state_), "%s", state->valuestring);
    auto str_cp = [&](const char* key, char* dst, size_t cap) {
        cJSON* v = cJSON_GetObjectItem(attrs, key);
        if (cJSON_IsString(v) && v->valuestring) snprintf(dst, cap, "%s", v->valuestring);
        else dst[0] = '\0';
    };
    str_cp("media_title", title_, sizeof(title_));
    str_cp("media_artist", artist_, sizeof(artist_));
    cJSON* vol = cJSON_GetObjectItem(attrs, "volume_level");
    if (cJSON_IsNumber(vol)) volume_ = (float)vol->valuedouble;
    cJSON* pos = cJSON_GetObjectItem(attrs, "media_position");
    cJSON* dur = cJSON_GetObjectItem(attrs, "media_duration");
    cJSON* pos_at = cJSON_GetObjectItem(attrs, "media_position_updated_at");
    ha_pos_ = (pos != nullptr && cJSON_IsNumber(pos)) ? pos->valuedouble : 0;
    dur_min_ = (dur != nullptr && cJSON_IsNumber(dur)) ? (float)(dur->valuedouble / 60.0) : 0;
    ha_at_epoch_ = 0;
    if (cJSON_IsString(pos_at) && pos_at->valuestring) {
        time_t at = 0;
        if (parse_ha_utc_timestamp(pos_at->valuestring, &at) && at > 0) {
            ha_at_epoch_ = (double)at;
        }
    }
    cJSON_Delete(root);
    // UI(500ms timer)在 LVGL task 读这些成员;32 位 CPU 上 double 写非原子,
    // 必须持 LVGL 锁写入,防止撕裂读导致进度计算出垃圾值
    lvgl_port_lock(pdMS_TO_TICKS(1000));
    valid_ = true;
    // 新鲜推送(<=90 秒)记录为基准;过旧推送不覆盖(继续本地推进)
    if (ha_at_epoch_ > 0) {
        double e = difftime(time(NULL), (time_t)ha_at_epoch_);
        if (e >= 0 && e <= 90) {
            last_good_pos_ = ha_pos_;
            last_good_time_ = (double)time(NULL);
        }
    }
    ESP_LOGI(TAG, "ha=%.0f at=%.0f(%s) dur=%.0f st=%s t=%.16s",
             ha_pos_, ha_at_epoch_,
             ha_at_epoch_ > 0 ? "ok" : "FAIL",
             dur_min_ * 60, state_, title_);
    lvgl_port_unlock();
    return true;
}

void PodApp::SendCommand(const char* service) {
    if (!ha_post(service, nullptr)) {
        ESP_LOGW(TAG, "命令失败: %s", service);
        if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
            lv_label_set_text(status_label_, "命令失败(检查音箱)");
            lvgl_port_unlock();
        }
    }
}

void PodApp::ApplyVolume(int delta) {
    float v = volume_ + delta * 0.05f;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    volume_ = v;
    char fields[64];
    snprintf(fields, sizeof(fields), "\"volume_level\": %.2f", (double)v);
    ha_post("volume_set", fields);
}

// ---------------- 后台 task ----------------

void PodApp::TaskEntry(void* arg) { static_cast<PodApp*>(arg)->Run(); }

void PodApp::Run() {
    // 立即拉一次,随后 3 秒轮询;每轮先消费命令
    for (;;) {
        int c = cmd_.exchange(0);
        switch (c) {
            case 1: SendCommand("media_play_pause"); break;
            case 2: ApplyVolume(+1); break;
            case 3: ApplyVolume(-1); break;
            case 4: SendCommand("media_previous_track"); break;
            case 5: SendCommand("media_next_track"); break;
            default: break;
        }
        FetchStatus();
        if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
            UpdateUi();
            lvgl_port_unlock();
        }
        // 3 秒(分片;退出立即生效)
        for (int i = 0; i < 30 && running_.load(); i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (!running_.load()) break;
    }
    vTaskDelete(NULL);
}

// ---------------- UI ----------------

void PodApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 顶栏:Pod + 播放状态
    status_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(status_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(C_FG), 0);
    lv_label_set_text(status_label_, "Pod · …");
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 12);

    // 曲名(大,长文本滚动)
    title_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(title_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_hex(C_FG), 0);
    lv_obj_set_width(title_label_, LV_HOR_RES - 24);
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(title_label_, "…");
    lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, 66);

    // 艺术家
    artist_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(artist_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(artist_label_, lv_color_hex(C_DIM), 0);
    lv_obj_set_width(artist_label_, LV_HOR_RES - 24);
    lv_obj_set_style_text_align(artist_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(artist_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(artist_label_, "");
    lv_obj_align(artist_label_, LV_ALIGN_TOP_MID, 0, 100);

    // 进度条
    pos_bar_ = lv_bar_create(root_);
    lv_obj_set_size(pos_bar_, LV_HOR_RES - 48, 6);
    lv_obj_set_style_bg_color(pos_bar_, lv_color_hex(C_HINT), 0);
    lv_obj_set_style_bg_opa(pos_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pos_bar_, 0, 0);
    lv_obj_set_style_border_width(pos_bar_, 0, 0);
    lv_obj_set_style_pad_all(pos_bar_, 0, 0);
    lv_obj_set_style_bg_color(pos_bar_, lv_color_hex(C_FG), LV_PART_INDICATOR);
    lv_obj_set_style_radius(pos_bar_, 0, LV_PART_INDICATOR);
    lv_bar_set_range(pos_bar_, 0, 100);
    lv_obj_align(pos_bar_, LV_ALIGN_TOP_MID, 0, 180);

    pos_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(pos_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(pos_label_, lv_color_hex(C_DIM), 0);
    lv_label_set_text(pos_label_, "");
    lv_obj_align(pos_label_, LV_ALIGN_TOP_MID, 0, 194);

    // 音量条 + 音量值
    vol_bar_ = lv_bar_create(root_);
    lv_obj_set_size(vol_bar_, LV_HOR_RES - 48, 10);
    lv_obj_set_style_bg_color(vol_bar_, lv_color_hex(C_HINT), 0);
    lv_obj_set_style_bg_opa(vol_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vol_bar_, 0, 0);
    lv_obj_set_style_border_width(vol_bar_, 0, 0);
    lv_obj_set_style_pad_all(vol_bar_, 0, 0);
    lv_obj_set_style_bg_color(vol_bar_, lv_color_hex(C_FG), LV_PART_INDICATOR);
    lv_obj_set_style_radius(vol_bar_, 0, LV_PART_INDICATOR);
    lv_bar_set_range(vol_bar_, 0, 100);
    lv_obj_align(vol_bar_, LV_ALIGN_TOP_MID, 0, 230);

    vol_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(vol_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(vol_label_, lv_color_hex(C_FG), 0);
    lv_label_set_text(vol_label_, "--");
    lv_obj_align(vol_label_, LV_ALIGN_TOP_MID, 0, 250);

    // 底部提示
    lv_obj_t* hint = lv_label_create(root_);
    lv_obj_set_style_text_font(hint, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_HINT), 0);
    lv_label_set_text(hint, "上下 音量 · OK 播放/暂停 · 长按 返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    ui_timer_ = lv_timer_create([](lv_timer_t* t) {
        PodApp* app = static_cast<PodApp*>(lv_timer_get_user_data(t));
        // 每 500ms:播放中匀速 +0.5 秒(钟表模式,视觉平滑)
        if (app->active_ && strcmp(app->state_, "playing") == 0) {
            app->display_pos_ += 0.5;
        }
        // 校正:与真实计算值偏差 >5 秒时跳正(防漂移/切歌/暂停恢复)
        double computed = app->ComputeTruePos();
        if (app->active_ && computed >= 0 &&
            fabs(computed - app->display_pos_) > 5.0) {
            app->display_pos_ = computed;
        }
        app->UpdateUi();
    }, 500, this);
    lv_timer_pause(ui_timer_);
}

double PodApp::ComputeTruePos() {
    if (strcmp(state_, "playing") != 0) {
        return ha_pos_;   // paused/idle:HA 值
    }
    double now_epoch = (double)time(NULL);
    if (now_epoch < 1600000000.0) return -1;   // SNTP 未同步
    // 切歌检测:title 变化 → 旧推送(位置/时刻)全是上一首的,不可信;
    // 直接从 0 起步本地推进,等 MA 推新数据后校准分支自然修正
    if (strcmp(title_, last_title_) != 0) {
        snprintf(last_title_, sizeof(last_title_), "%s", title_);
        last_good_pos_ = 0;
        last_good_time_ = now_epoch;
        return 0;
    }
    double e = (ha_at_epoch_ > 0) ? (now_epoch - ha_at_epoch_) : -1;
    if (e >= 0 && e <= 90) {
        double p = ha_pos_ + e;   // 新鲜:校准
        last_good_pos_ = p;
        last_good_time_ = now_epoch;
        return p;
    }
    if (last_good_time_ > 0) {
        return last_good_pos_ + (now_epoch - last_good_time_);   // 本地推进
    }
    return -1;
}

void PodApp::UpdateUi() {
    if (root_ == nullptr) return;
    if (!valid_) {
        lv_label_set_text(status_label_, "Pod · 连接中…");
        return;
    }
    const char* state_cn = strcmp(state_, "playing") == 0 ? "播放中"
                          : strcmp(state_, "paused") == 0 ? "已暂停"
                          : strcmp(state_, "idle") == 0 ? "空闲" : state_;
    lv_label_set_text_fmt(status_label_, "Pod · %s", state_cn);
    lv_label_set_text(title_label_, title_[0] ? title_ : "(无曲目)");
    lv_label_set_text(artist_label_, artist_);
    lv_bar_set_value(vol_bar_, (int32_t)(volume_ * 100), LV_ANIM_OFF);
    lv_label_set_text_fmt(vol_label_, "音量 %d%%", (int)(volume_ * 100));
    // 显示位置:display_pos_ 由 500ms timer 匀速推进,此处只渲染
    double cur = display_pos_;
    double dur_s = dur_min_ * 60.0;
    if (dur_s > 1 && cur > dur_s) cur = dur_s;
    float pos_min_ = (float)(cur / 60.0);
    if (dur_min_ > 0.01f) {
        int pct = (int32_t)(pos_min_ * 100 / dur_min_);
        lv_bar_set_value(pos_bar_, pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(pos_label_, "%d:%02d / %d:%02d",
                              (int)pos_min_, (int)((pos_min_ - (int)pos_min_) * 60),
                              (int)dur_min_, (int)((dur_min_ - (int)dur_min_) * 60));
    } else {
        lv_bar_set_value(pos_bar_, 0, LV_ANIM_OFF);
        lv_label_set_text(pos_label_, "");
    }
}

void PodApp::HandleKey(int key) {
    if (!active_) return;
    switch (key) {
        case 0: cmd_.store(2); break;   // 上=音量+
        case 1: cmd_.store(3); break;   // 下=音量-
        case 2: cmd_.store(1); break;   // OK=播放/暂停
        case 4: cmd_.store(4); break;   // 长按上=上一首
        case 5: cmd_.store(5); break;   // 长按下=下一首
        default: break;                 // 长按OK=退出(board 处理)
    }
}

void PodApp::Enter() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && !active_) {
            lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
            active_ = true;
            if (ui_timer_ != nullptr) lv_timer_resume(ui_timer_);
        }
        lvgl_port_unlock();
    }
    if (!running_.load()) {
        running_.store(true);
        if (xTaskCreate(TaskEntry, "pod", 8192, this, 3, nullptr) != pdPASS) {
            running_.store(false);
            ESP_LOGE(TAG, "sonos task 创建失败");
        }
    }
}

void PodApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        lvgl_port_unlock();
    }
}

void PodApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && active_) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            active_ = false;
            if (ui_timer_ != nullptr) lv_timer_pause(ui_timer_);
        }
        lvgl_port_unlock();
    }
    running_.store(false);   // task 3 秒内自行退出并自杀
}
