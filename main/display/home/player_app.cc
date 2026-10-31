// player_app —— Sonos + HomePod 统一播放控制器。见 player_app.h。
// 逻辑融合自 sonos_app/pod_app(已参数化),保留已验证的进度算法。

#include "player_app.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "home_screen.h"

LV_FONT_DECLARE(font_home_cn_16);

static const char* TAG = "player";
static constexpr int kCoverSize = 64;   // 与 hermes _cover_art._SIZE 同步(字节数精确匹配)。曾试 80:
                                        // 播放期堆最大连续块仅 ~5120B,7424B 缓冲分配必败→无封面,已回退
static constexpr int kCoverPaletteBytes = 256 * 4;
static constexpr int kCoverDataBytes = kCoverPaletteBytes + (kCoverSize * kCoverSize);

// 歌词代理 = radio_relay 同机同端口(radio_app kStations 同款硬编码)
static const char* kRelayBase = "http://192.168.1.200:8769";

// 百分号编码(HA 曲名/歌手为原始 UTF-8,需转义进 query)
static void url_encode(const char* in, char* out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p != '\0' && o + 3 < cap; p++) {
        unsigned char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

// 码点写 UTF-8(歌词 JSON 反转义用);缓冲不足返回 false 由调用方截断
static bool utf8_put(char** dst, char* end, uint32_t cp) {
    if (cp < 0x80) {
        if (*dst >= end) return false;
        *(*dst)++ = (char)cp;
        return true;
    }
    if (cp < 0x800) {
        if (end - *dst < 2) return false;
        *(*dst)++ = (char)(0xC0 | (cp >> 6));
        *(*dst)++ = (char)(0x80 | (cp & 0x3F));
        return true;
    }
    if (cp < 0x10000) {
        if (end - *dst < 3) return false;
        *(*dst)++ = (char)(0xE0 | (cp >> 12));
        *(*dst)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*dst)++ = (char)(0x80 | (cp & 0x3F));
        return true;
    }
    if (end - *dst < 4) return false;
    *(*dst)++ = (char)(0xF0 | (cp >> 18));
    *(*dst)++ = (char)(0x80 | ((cp >> 12) & 0x3F));
    *(*dst)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
    *(*dst)++ = (char)(0x80 | (cp & 0x3F));
    return true;
}

// LRC 时间标签 [mm:ss.ff] 手工解析(不用 sscanf %f——本项目有 %f 前科);
// 顺带拒绝 [ti:]/[ar:]/[offset:] 等元数据标签(非数字/含字母即 false)
static bool parse_lrc_time(const char* s, const char* e, float* out) {
    int mm = 0, ss = 0;
    const char* p = s;
    while (p < e && *p >= '0' && *p <= '9') {
        mm = mm * 10 + (*p - '0');
        if (mm > 199) return false;
        p++;
    }
    if (p == s || p >= e || *p != ':') return false;
    p++;
    const char* ss_start = p;
    while (p < e && *p >= '0' && *p <= '9') {
        ss = ss * 10 + (*p - '0');
        if (ss > 99) return false;
        p++;
    }
    if (p == ss_start) return false;
    float frac = 0;
    if (p < e && *p == '.') {
        p++;
        const char* fs = p;
        float scale = 0.1f;
        while (p < e && *p >= '0' && *p <= '9') {
            frac += (*p - '0') * scale;
            scale *= 0.1f;
            p++;
        }
        if (p == fs) return false;
    }
    if (p != e) return false;
    *out = mm * 60.0f + ss + frac;
    return true;
}

// ---------------- HA HTTP 辅助 ----------------

static bool ha_get(const char* base_url, const char* token, const char* entity_id,
                   char* body, size_t cap) {
    static char url[160];
    snprintf(url, sizeof(url), "%s/api/states/%s", base_url, entity_id);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 6000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return false;
    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", auth);
    int len = 0;
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
    ESP_LOGI(TAG, "ha_get: ok=%d len=%d code=%d", ok ? 1 : 0, len,
             ok ? esp_http_client_get_status_code(client) : -1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static bool ha_post(const char* base_url, const char* token, const char* entity_id,
                    const char* service, const char* extra_fields) {
    // perform 一体化(M5-DIAL 同款):手动 open/fetch 流式 POST 在
    // IDF6 上状态码拿不到(实测全 FAIL),perform 内部收完响应。
    static char url[192], body[192], auth[256];
    snprintf(url, sizeof(url), "%s/api/services/media_player/%s", base_url, service);
    if (extra_fields && extra_fields[0]) {
        snprintf(body, sizeof(body), "{\"entity_id\": \"%s\", %s}", entity_id, extra_fields);
    } else {
        snprintf(body, sizeof(body), "{\"entity_id\": \"%s\"}", entity_id);
    }
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return false;
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && code >= 200 && code < 300;
}

static bool ha_update_entity(const char* base_url, const char* token, const char* entity_id) {
    // 外部控制器切歌时，Sonos 集成可能按约一分钟的节奏扫描。
    // homeassistant.update_entity 可同步触发本实体更新，随后再读状态。
    char url[192], body[128], auth[256];
    snprintf(url, sizeof(url), "%s/api/services/homeassistant/update_entity", base_url);
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity_id);
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 6000;
    cfg.buffer_size = 512;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return false;
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && code >= 200 && code < 300;
}

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

// ---------------- App ----------------

PlayerApp& PlayerApp::Get() {
    static PlayerApp inst;
    return inst;
}

void PlayerApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_center(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101418), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 8, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // tab 栏(顶部两个纯文本标签,金融 app 同款:无按钮框,选中高亮)
    for (int i = 0; i < kPlayers; i++) {
        tab_lbl_[i] = lv_label_create(root_);
        lv_obj_set_style_text_font(tab_lbl_[i], &font_home_cn_16, 0);
        lv_obj_set_style_text_color(tab_lbl_[i], lv_color_hex(0x4A545E), 0);
        lv_label_set_text(tab_lbl_[i], i == 0 ? "Sonos" : "HomePod");
        lv_obj_align(tab_lbl_[i], LV_ALIGN_TOP_MID, i == 0 ? -56 : 56, 8);
    }

    // 专辑封面：服务器已转为 LVGL 原生 I8；设备只在 APP 打开后申请 5120B。
    cover_img_ = lv_image_create(root_);
    lv_obj_set_size(cover_img_, kCoverSize, kCoverSize);
    // 布局(240×320,塞入双行歌词): tab 8-27 / 封面 32-96 / 曲名 102 /
    // 艺术家 124 / 歌词 166(单行) / 进度条 230 / 时间 242 / 底部帮助
    lv_obj_align(cover_img_, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_add_flag(cover_img_, LV_OBJ_FLAG_HIDDEN);

    // 状态/曲名/艺术家
    // 曲名（封面下方）
    title_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(title_label_, &font_home_cn_16, 0);   // 中英混排
    lv_obj_set_style_text_color(title_label_, lv_color_hex(0x9FE870), 0);
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title_label_, LV_HOR_RES - 24);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(title_label_, "…");
    lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, 102);

    // 演唱者(曲名下方)
    artist_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(artist_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(artist_label_, lv_color_hex(0xE8E3D3), 0);
    lv_obj_set_style_text_align(artist_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(artist_label_, LV_HOR_RES - 24);
    lv_label_set_long_mode(artist_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(artist_label_, "");
    lv_obj_align(artist_label_, LV_ALIGN_TOP_MID, 0, 124);

    // 歌词双行: 当前行亮 + 下一行暗(TickProgress 随播放位置推进)
    lyric_now_ = lv_label_create(root_);
    lv_obj_set_style_text_font(lyric_now_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(lyric_now_, lv_color_hex(0xE8E3D3), 0);
    lv_obj_set_style_text_align(lyric_now_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lyric_now_, LV_HOR_RES - 24);
    lv_label_set_long_mode(lyric_now_, LV_LABEL_LONG_DOT);
    lv_label_set_text(lyric_now_, "");
    lv_obj_align(lyric_now_, LV_ALIGN_TOP_MID, 0, 166);

    // 进度条 + 时间(M5-DIAL 同款钟表式推进,500ms timer 平滑)
    pos_bar_ = lv_bar_create(root_);
    lv_obj_set_size(pos_bar_, 200, 4);
    lv_obj_align(pos_bar_, LV_ALIGN_TOP_MID, 0, 230);
    lv_bar_set_range(pos_bar_, 0, 100);
    lv_bar_set_value(pos_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(pos_bar_, lv_color_hex(0x222A31), 0);
    lv_obj_set_style_bg_opa(pos_bar_, LV_OPA_COVER, 0);
    {
        static lv_style_t ind2;
        static bool inited2 = false;
        if (!inited2) {
            lv_style_init(&ind2);
            lv_style_set_bg_color(&ind2, lv_color_hex(0xFFC24B));
            lv_style_set_bg_opa(&ind2, LV_OPA_COVER);
            lv_style_set_radius(&ind2, 3);
            inited2 = true;
        }
        lv_obj_add_style(pos_bar_, &ind2, LV_PART_INDICATOR);
    }
    // (进度时间 label 在 h 里加 prog_label_)
    // 500ms 平滑推进 timer(M5-DIAL 同款节奏)
    ui_timer_ = lv_timer_create([](lv_timer_t* t) {
        static_cast<PlayerApp*>(lv_timer_get_user_data(t))->TickProgress();
    }, 500, this);
    lv_timer_pause(ui_timer_);

    // 进度时间 label(此前丢失创建导致 TickProgress 崩溃重启)
    prog_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(prog_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(prog_label_, lv_color_hex(0xAAB4BE), 0);
    lv_label_set_text(prog_label_, "");
            lv_obj_align(prog_label_, LV_ALIGN_TOP_MID, 0, 242);

    // 底部操作说明
    pos_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(pos_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(pos_label_, lv_color_hex(0x8A949E), 0);
    lv_obj_set_style_text_align(pos_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(pos_label_, "上下:音量 长按:切歌\n双击OK:切换设备");
    lv_obj_align(pos_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_long_mode(pos_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(pos_label_, LV_HOR_RES - 24);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void PlayerApp::TaskEntry(void* arg) {
    static_cast<PlayerApp*>(arg)->Run();
}

void PlayerApp::Run() {
    static const PlayerCfg cfgs[kPlayers] = {
        {SONOS_HA_BASE_URL, SONOS_HA_TOKEN, SONOS_ENTITY_ID, "Sonos"},
        {POD_HA_BASE_URL, POD_HA_TOKEN, POD_ENTITY_ID, "HomePod"},
    };
    for (int i = 0; i < kPlayers; i++) {
        st_[i] = PlayerState{};
    }
    int64_t last_entity_refresh_us = 0;
    int last_entity_refresh_idx = -1;
    // 封面缓冲在首个 HTTP 之前预分配:之后堆被 HA 客户端切碎,largest 常
    // 只剩 ~5120,与 64×64 缓冲(5120B)正好同量级,晚分配容易失败
    if (cover_data_ == nullptr) {
        cover_data_ = static_cast<uint8_t*>(heap_caps_malloc(kCoverDataBytes, MALLOC_CAP_8BIT));
        if (cover_data_ == nullptr) ESP_LOGW(TAG, "cover buffer prealloc failed (%dB)", kCoverDataBytes);
    }
    // 歌词文本缓冲同理(典型一首歌 LRC ≤3KB;分配失败仅无歌词,不致命)
    if (lyr_buf_ == nullptr) {
        lyr_buf_ = static_cast<uint8_t*>(heap_caps_malloc(kLyrBufBytes, MALLOC_CAP_8BIT));
        if (lyr_buf_ == nullptr) ESP_LOGW(TAG, "lyrics buffer prealloc failed (%dB)", kLyrBufBytes);
    }
    for (;;) {
        int c = cmd_.exchange(0);
        int t = cmd_target_.load();
        if (c != 0) ESP_LOGI(TAG, "run cmd %d -> player %d", c, t);
        switch (c) {
            case 1: ESP_LOGI(TAG, "play_pause %s", ha_post(cfgs[t].base_url, cfgs[t].token, cfgs[t].entity_id, "media_play_pause", nullptr) ? "OK" : "FAIL"); break;
            case 2: ApplyVolume(t, 5); break;
            case 3: ApplyVolume(t, -5); break;
            case 4: ESP_LOGI(TAG, "prev %s", ha_post(cfgs[t].base_url, cfgs[t].token, cfgs[t].entity_id, "media_previous_track", nullptr) ? "OK" : "FAIL"); break;
            case 5: ESP_LOGI(TAG, "next %s", ha_post(cfgs[t].base_url, cfgs[t].token, cfgs[t].entity_id, "media_next_track", nullptr) ? "OK" : "FAIL"); break;
            default: break;
        }
        // 固定本轮 tab 快照。cur_ 同时被 LVGL/按键上下文写入，不能在一次
        // HTTP/封面/UI 流程中反复读取，否则切 TAB 时会出现 Sonos 内容串到 HomePod。
        const int active_idx = cur_.load();
        // 不依赖 Sonos 集成默认的约一分钟扫描：当前设备首次进入/切 TAB
        // 以及之后每 6 秒请求 HA 立即更新实体。完成后短暂等待再 FetchStatus。
        int64_t now_us = esp_timer_get_time();
        if (active_idx != last_entity_refresh_idx ||
            now_us - last_entity_refresh_us >= 6000000) {
            bool refreshed = ha_update_entity(cfgs[active_idx].base_url, cfgs[active_idx].token,
                                              cfgs[active_idx].entity_id);
            ESP_LOGI(TAG, "entity refresh p%d %s", active_idx, refreshed ? "OK" : "FAIL");
            last_entity_refresh_us = now_us;
            last_entity_refresh_idx = active_idx;
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        // 两个播放器都轮询（切 tab 即有数据）；当前 tab 每 3s，另一个 15s。
        FetchStatus(active_idx);
        static int slow = 0;
        if (++slow >= 5) {
            slow = 0;
            FetchStatus(1 - active_idx);
        }
        // 外部设备切歌时，HA 的 title 往往先于 entity_picture 到达。
        // 在 2/4/6 秒各重试一次，收到新封面后覆盖旧缓冲。
        if (cover_requested_.exchange(false)) {
            FetchCover(active_idx);
            if (cover_retries_left_ > 0) {
                cover_retry_due_us_ = esp_timer_get_time() + 2000000;
            }
        } else if (cover_retries_left_ > 0 && esp_timer_get_time() >= cover_retry_due_us_) {
            --cover_retries_left_;
            FetchCover(active_idx);
            cover_retry_due_us_ = esp_timer_get_time() + 2000000;
        }
        // 切歌/切 tab 的歌词请求(在途不做重试:下一次 title 变化会再触发)
        if (lyr_requested_.exchange(false)) {
            FetchLyrics(active_idx);
        }
        // 冷查询超时后的同曲重试(FetchLyrics 失败时排定)
        if (lyr_retry_due_us_ != 0 && esp_timer_get_time() >= lyr_retry_due_us_) {
            lyr_retry_due_us_ = 0;
            lyr_requested_.store(true);
        }
        // Leave/Destroy 可能在 HTTP 期间发生；退出后不再触碰 UI。
        if (!running_.load()) break;
        if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
            UpdateUi();
            lvgl_port_unlock();
        }
        // 3s 轮询间隔,但有按键命令立即醒来处理——原来固定睡满 3s,
        // 按键要等整轮才被执行(实测延迟 2.6-3.1s,"像定住了")
        for (int i = 0; i < 30 && running_.load() && cmd_.load() == 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!running_.load()) break;
    }
    ResetCover();
    // 歌词缓冲与封面同生命周期:任务收尾释放,不为下一次进入留残量
    if (lyr_buf_ != nullptr) {
        heap_caps_free(lyr_buf_);
        lyr_buf_ = nullptr;
    }
    lyr_count_ = 0;
    lyr_player_ = -1;
    lyr_key_title_[0] = '\0';
    lyr_retry_due_us_ = 0;
    lyr_attempts_ = 0;
    lyr_attempt_title_[0] = '\0';
    vTaskDelete(NULL);
}

bool PlayerApp::FetchStatus(int idx) {
    static const PlayerCfg cfgs[kPlayers] = {
        {SONOS_HA_BASE_URL, SONOS_HA_TOKEN, SONOS_ENTITY_ID, "Sonos"},
        {POD_HA_BASE_URL, POD_HA_TOKEN, POD_ENTITY_ID, "HomePod"},
    };
    static char body[4096];
    if (!ha_get(cfgs[idx].base_url, cfgs[idx].token, cfgs[idx].entity_id, body, sizeof(body))) {
        ESP_LOGE(TAG, "FetchStatus p%d ha_get FAIL", idx);
        return false;
    }
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return false;
    cJSON* attrs = cJSON_GetObjectItem(root, "attributes");
    if (!cJSON_IsObject(attrs)) {
        cJSON_Delete(root);
        return false;
    }
    PlayerState& ps = st_[idx];
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        cJSON* state = cJSON_GetObjectItem(root, "state");
        snprintf(ps.state, sizeof(ps.state), "%s",
                 cJSON_IsString(state) && state->valuestring ? state->valuestring : "idle");
        cJSON* vol = cJSON_GetObjectItem(attrs, "volume_level");
        ps.volume = (cJSON_IsNumber(vol)) ? vol->valuedouble : 0;
        cJSON* title = cJSON_GetObjectItem(attrs, "media_title");
        snprintf(ps.title, sizeof(ps.title), "%s",
                 cJSON_IsString(title) && title->valuestring ? title->valuestring : "");
        cJSON* artist = cJSON_GetObjectItem(attrs, "media_artist");
        snprintf(ps.artist, sizeof(ps.artist), "%s",
                 cJSON_IsString(artist) && artist->valuestring ? artist->valuestring : "");
        cJSON* picture = cJSON_GetObjectItem(attrs, "entity_picture");
        snprintf(ps.picture_url, sizeof(ps.picture_url), "%s",
                 cJSON_IsString(picture) && picture->valuestring ? picture->valuestring : "");
        cJSON* dur = cJSON_GetObjectItem(attrs, "media_duration");
        
        double dur_s = cJSON_IsNumber(dur) ? dur->valuedouble : 0;
        ps.dur_min = (float)(dur_s / 60.0);
        cJSON* pos = cJSON_GetObjectItem(attrs, "media_position");
        cJSON* pos_at = cJSON_GetObjectItem(attrs, "media_position_updated_at");
        ps.ha_pos = (pos != nullptr && cJSON_IsNumber(pos)) ? pos->valuedouble : 0;
        ps.ha_at_epoch = 0;
        if (cJSON_IsString(pos_at) && pos_at->valuestring) {
            time_t at = 0;
            if (parse_ha_utc_timestamp(pos_at->valuestring, &at) && at > 0) {
                ps.ha_at_epoch = (double)at;
            }
        }
        // 时间戳校正(Tab5 同款 HA 标准算法,2026-10-31 歌词错位实锤):
        // media_position 是 updated_at 时刻的快照,真实值 = position +
        // (now - updated_at);不校正则校准逻辑反复把进度拽回旧值。
        // 仅 SNTP 已同步且年龄 [0,300]s 才校正——保留单调钟锚对时钟偏差
        // 的免疫(设备时钟未同步时 wall clock 减出负数的老坑)。
        double ha_live = ps.ha_pos;
        if (ps.ha_at_epoch > 0) {
            time_t now = time(NULL);
            if (now > 1767225600) {   // 2026-01-01 之后=SNTP 已同步
                double age = difftime(now, (time_t)ps.ha_at_epoch);
                if (age >= 0 && age <= 300) ha_live = ps.ha_pos + age;
            }
        }
        // 单调时钟锚(免疫设备/HA 时钟偏差):切歌重置,同曲 pos 前进则校准
        {
            bool title_changed = strcmp(ps.title, ps.last_title) != 0;
            bool picture_changed = strcmp(ps.picture_url, ps.last_picture_url) != 0;
            memmove(ps.last_title, ps.title, strnlen(ps.title, sizeof(ps.last_title) - 1) + 1);
            memmove(ps.last_picture_url, ps.picture_url,
                    strnlen(ps.picture_url, sizeof(ps.last_picture_url) - 1) + 1);
            int64_t mono = esp_timer_get_time();
            if (title_changed) {
                ps.display_pos = ha_live;        // 新歌:从服务端实时位置起步
                ps.anchor_us_ = mono;
            } else {
                // 与"当前实时估算"比较(不是过期的 display_pos——
                // 那个恒为上次锚定值,MA 恒 0 时每轮都误重置弹回起点)
                double live = ps.display_pos;
                if (ps.anchor_us_ > 0 && strcmp(ps.state, "playing") == 0) {
                    live += (double)(mono - ps.anchor_us_) / 1000000.0;
                }
                // 服务端实时位置落在本地估算邻域才校准;MA 恒 0 追不上→本地走
                if (ha_live >= live - 2.0 && ha_live <= live + 30.0) {
                    ps.display_pos = ha_live;
                    ps.anchor_us_ = mono;
                }
            }
            // 外部切歌时 title 和 entity_picture 会分两次抵达：
            // 任一变化都启动重试，picture 后到也会覆盖先前可能拿到的旧封面。
            if (idx == cur_.load() && (title_changed || picture_changed)) {
                cover_ready_ = false;
                cover_retries_left_ = 3;
                cover_retry_due_us_ = 0;
                cover_requested_.store(true);
                if (title_changed) lyr_requested_.store(true);   // 歌词随曲名走
            }
        }
        cJSON_Delete(root);
        ps.valid = true;
        lvgl_port_unlock();
    }
    return true;
}

bool PlayerApp::FetchCover(int idx) {
    if (!running_.load()) return false;
    // 不使用静态大数组：仅在 Player APP task 内按需申请，失败即降级为无封面。
    if (cover_data_ == nullptr) {
        cover_data_ = static_cast<uint8_t*>(heap_caps_malloc(kCoverDataBytes, MALLOC_CAP_8BIT));
        if (cover_data_ == nullptr) {
            ESP_LOGW(TAG, "cover skipped: cannot allocate %dB", kCoverDataBytes);
            return false;
        }
    }

    esp_http_client_config_t cfg = {};
    cfg.url = idx == 0 ? HOME_AGENDA_BASE_URL "/assets/cover.i8?player=sonos"
                      : HOME_AGENDA_BASE_URL "/assets/cover.i8?player=pod";
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 512;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return false;

    int length = 0;
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int read = 0;
        while (length < kCoverDataBytes &&
               (read = esp_http_client_read(client, reinterpret_cast<char*>(cover_data_) + length,
                                             kCoverDataBytes - length)) > 0) {
            length += read;
        }
        ok = esp_http_client_get_status_code(client) == 200 && length == kCoverDataBytes;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok || !running_.load()) {
        ESP_LOGW(TAG, "cover unavailable: bytes=%d expected=%d", length, kCoverDataBytes);
        return false;
    }

    cover_dsc_ = {};
    cover_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    cover_dsc_.header.cf = LV_COLOR_FORMAT_I8;
    cover_dsc_.header.w = kCoverSize;
    cover_dsc_.header.h = kCoverSize;
    cover_dsc_.header.stride = kCoverSize;
    cover_dsc_.data_size = kCoverDataBytes;
    cover_dsc_.data = cover_data_;
    cover_player_ = idx;
    cover_ready_ = true;
    ESP_LOGI(TAG, "cover ready: %dB", length);
    return true;
}

void PlayerApp::ResetCover() {
    cover_ready_ = false;
    cover_player_ = -1;
    cover_retries_left_ = 0;
    cover_retry_due_us_ = 0;
    cover_requested_.store(false);
    if (cover_data_ != nullptr) {
        heap_caps_free(cover_data_);
        cover_data_ = nullptr;
    }
    cover_dsc_ = {};
}

// ── 歌词: radio_relay /lyrics 的 MA/LRCLIB 代理,流式就地解析 ──
// 响应 JSON 的 synced 值里中文是 \uXXXX 转义(python json.dumps 默认
// ensure_ascii),逐块原地反转义(写指针恒≤读指针),"synced" 值闭合即
// 不再解析(后面的 plain 读走丢弃,只为中继日志干净)。紧凑存储:
// 行文本压实进 lyr_buf_ 头部 + 升序时间/偏移数组,一首中文歌 ~3KB。
void PlayerApp::FetchLyrics(int idx) {
    if (!running_.load() || lyr_buf_ == nullptr) return;
    PlayerState& ps = st_[idx];
    // 快照(与 FetchStatus 同任务串行,无竞态)
    char title[96], artist[64];
    strlcpy(title, ps.title, sizeof(title));
    strlcpy(artist, ps.artist, sizeof(artist));
    // 先失效旧词(旧词"定住"比无词难看;空曲名/失败路径都走这里)
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        lyr_count_ = 0;
        lyr_idx_ = -1;
        lvgl_port_unlock();
    }
    if (title[0] == '\0') {
        lyr_retry_due_us_ = 0;
        return;
    }
    if (strcmp(lyr_attempt_title_, title) != 0) {   // 换曲: 重试计数归零
        strlcpy(lyr_attempt_title_, title, sizeof(lyr_attempt_title_));
        lyr_attempts_ = 0;
    }
    static char et[96 * 3 + 1], ea[64 * 3 + 1];
    url_encode(title, et, sizeof(et));
    url_encode(artist, ea, sizeof(ea));
    int dur_s = ps.dur_min > 0.01f ? (int)(ps.dur_min * 60.0f) : 0;
    static char url[600];   // 最坏 26 基址+14+288 编码曲名+8+192 编码歌手+18 时长
    if (dur_s > 0) {
        snprintf(url, sizeof(url), "%s/lyrics?title=%s&artist=%s&duration=%d",
                 kRelayBase, et, ea, dur_s);
    } else {
        snprintf(url, sizeof(url), "%s/lyrics?title=%s&artist=%s", kRelayBase, et, ea);
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 512;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return;

    // 状态机: stage 0=找 "synced" 键 1=键后过渡(跳:与空白等开引号;容忍
    // JSON 分隔空格——Python json.dumps 默认输出 "synced": "…",2026-10-31
    // 实锤旧无空格匹配串全程落空,Tab5(nlohmann 完整解析)正常而本机无词)
    // 2=值内 3=完成/丢弃。键只可能首个命中——JSON 字符串值内的引号必被
    // 转义,假匹配不可能。
    static const char kKey[] = "\"synced\"";
    int stage = 0;
    int mp = 0;          // 键匹配进度
    int esc = 0;         // 0=普通 1=刚见反斜杠 2=收集 \u 十六进制
    int ucnt = 0;
    uint32_t uacc = 0;
    bool have_hi = false;   // \u 代理对高半在途
    uint32_t hi = 0;
    char* dst = reinterpret_cast<char*>(lyr_buf_);
    char* dst_end = dst + kLyrBufBytes - 1;
    bool ok = false;
    char chunk[256];
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int n;
        int discarded = 0;
        while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
            if (stage == 3) {   // 值已收完: 丢弃余量(含 plain 字段)
                discarded += n;
                if (discarded > 65536) break;
                continue;
            }
            for (int i = 0; i < n; i++) {
                char c = chunk[i];
                if (stage == 0) {
                    if (c == kKey[mp]) {
                        if (++mp == (int)sizeof(kKey) - 1) stage = 1;
                    } else {
                        mp = (c == kKey[0]) ? 1 : 0;
                    }
                    continue;
                }
                if (stage == 1) {   // 键后过渡: 等 : 空白 与开引号
                    if (c == ':' || c == ' ' || c == '\t') continue;
                    if (c == '"') { stage = 2; continue; }   // 开引号: 进值内
                    // 非 string 值(如 null)——本机无同步词,放弃
                    stage = 3;
                    break;
                }
                // stage == 2: 值内
                bool full = false;
                if (esc == 1) {
                    esc = 0;
                    if (c == 'n') full = !utf8_put(&dst, dst_end, '\n');
                    else if (c == 't') full = !utf8_put(&dst, dst_end, '\t');
                    else if (c == '"') full = !utf8_put(&dst, dst_end, '"');
                    else if (c == '\\') full = !utf8_put(&dst, dst_end, '\\');
                    else if (c == '/') full = !utf8_put(&dst, dst_end, '/');
                    else if (c == 'u') { esc = 2; ucnt = 0; uacc = 0; }
                    // r/b/f: 丢弃(\r 行尾本来就要去)
                } else if (esc == 2) {
                    int v = (c >= '0' && c <= '9') ? c - '0'
                          : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                          : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                    if (v >= 0 && ucnt < 4) {
                        uacc = (uacc << 4) | (uint32_t)v;
                        ucnt++;
                    }
                    if (ucnt >= 4 || v < 0) {
                        esc = 0;
                        uint32_t cp = (v >= 0 && ucnt >= 4) ? uacc : 0xFFFD;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {          // 高代理
                            if (have_hi) full = !utf8_put(&dst, dst_end, 0xFFFD);
                            have_hi = true;
                            hi = cp;
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {   // 低代理
                            if (have_hi) {
                                uint32_t c32 = 0x10000 + ((hi - 0xD800) << 10) + (cp - 0xDC00);
                                full = !utf8_put(&dst, dst_end, c32);
                                have_hi = false;
                            } else {
                                full = !utf8_put(&dst, dst_end, 0xFFFD);
                            }
                        } else {
                            if (have_hi) { full = !utf8_put(&dst, dst_end, 0xFFFD); have_hi = false; }
                            if (cp >= 0x20) full = !utf8_put(&dst, dst_end, cp);
                        }
                    }
                } else if (c == '\\') {
                    esc = 1;
                } else if (c == '"') {
                    stage = 3;   // 值闭合
                    ok = true;
                    break;
                } else {
                    full = !utf8_put(&dst, dst_end, (unsigned char)c);
                }
                if (full) {   // 缓冲满: 截断收词(比整首丢弃好)
                    stage = 2;
                    ok = true;
                    break;
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "lyrics open failed");
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok || !running_.load()) {
        if (ok) ESP_LOGW(TAG, "lyrics fetch aborted (leaving)");
        else ESP_LOGI(TAG, "lyrics p%d '%.32s': none (try %d)", idx, title, lyr_attempts_ + 1);
        // 冷查询偶发超时: 20s 后再试(中继缓存已热,二次秒回),最多 3 次
        if (lyr_attempts_ < 3) {
            lyr_attempts_++;
            lyr_retry_due_us_ = esp_timer_get_time() + 20000000;
        } else {
            lyr_retry_due_us_ = 0;
        }
        return;
    }
    *dst = '\0';

    // LRC 行抽取: 就地把行文本前移压实(写区恒在已读区之内——每行存走的
    // 字节 ≤ 该行原始长度,累积不等式保证不覆盖未读内容);同行多时间戳
    // 展开为多条共享同一偏移;插入排序保升序稳定(LRCLIB 偶有乱序多标签)
    float ts[kMaxLyrLines];
    uint16_t offs[kMaxLyrLines];
    int cnt = 0;
    uint16_t tlen = 0;
    char* line = reinterpret_cast<char*>(lyr_buf_);
    while (*line != '\0' && cnt < kMaxLyrLines) {
        char* eol = strchr(line, '\n');
        if (eol != nullptr) *eol = '\0';
        float times[8];
        int nt = 0;
        char* q = line;
        while (*q == '[' && nt < 8) {
            char* cl = strchr(q, ']');
            if (cl == nullptr) break;
            float t;
            if (parse_lrc_time(q + 1, cl, &t)) {
                times[nt++] = t;
                q = cl + 1;
            } else {
                break;   // [ti:]/[offset:] 等元数据标签: 整行放弃
            }
        }
        if (nt > 0) {
            char* e = q + strlen(q);
            while (e > q && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
            size_t tl = strlen(q);   // 空文本=前奏空白标记,也占位(到点清屏)
            if ((uint32_t)tlen + tl + 1 <= kLyrBufBytes) {
                uint16_t off = tlen;
                char* dstp = reinterpret_cast<char*>(lyr_buf_) + tlen;
                if (dstp != q) memmove(dstp, q, tl + 1);
                for (int k = 0; k < nt && cnt < kMaxLyrLines; k++) {
                    int j = cnt;
                    while (j > 0 && ts[j - 1] > times[k] + 0.001f) {
                        ts[j] = ts[j - 1];
                        offs[j] = offs[j - 1];
                        j--;
                    }
                    ts[j] = times[k];
                    offs[j] = off;
                    cnt++;
                }
                tlen += (uint16_t)(tl + 1);
            }
        }
        if (eol == nullptr) break;
        line = eol + 1;
    }
    if (cnt > 0 && lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        memcpy(lyr_t_, ts, sizeof(float) * cnt);
        memcpy(lyr_off_, offs, sizeof(uint16_t) * cnt);
        strlcpy(lyr_key_title_, title, sizeof(lyr_key_title_));
        lyr_player_ = idx;
        lyr_count_ = cnt;
        lvgl_port_unlock();
        lyr_attempts_ = 0;
        lyr_retry_due_us_ = 0;
        ESP_LOGI(TAG, "lyrics p%d '%.32s': %d lines", idx, title, cnt);
    } else if (cnt == 0) {
        lyr_retry_due_us_ = 0;   // 明确无词: 不重试(中继已负向缓存)
    }
}

void PlayerApp::SendCommand(int idx, const char* service) {
    cmd_target_.store(idx);
    (void)service;
}

void PlayerApp::ApplyVolume(int idx, int delta) {
    static const PlayerCfg cfgs[kPlayers] = {
        {SONOS_HA_BASE_URL, SONOS_HA_TOKEN, SONOS_ENTITY_ID, "Sonos"},
        {POD_HA_BASE_URL, POD_HA_TOKEN, POD_ENTITY_ID, "HomePod"},
    };
    ESP_LOGI(TAG, "ApplyVolume p%d %+d (cur %.2f)", idx, delta, st_[idx].volume);
    // 返回值在下方统一打
    int v = (int)(st_[idx].volume * 100) + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    char extra[48];
    snprintf(extra, sizeof(extra), "\"volume_level\": %.2f", v / 100.0);
    bool ok = ha_post(cfgs[idx].base_url, cfgs[idx].token, cfgs[idx].entity_id, "volume_set", extra);
    ESP_LOGI(TAG, "volume_set %s", ok ? "OK" : "FAILED");
}

void PlayerApp::SwitchTab(int idx) {
    if (idx == cur_.load()) return;
    cur_.store(idx);
    // 另一台播放器此前可能只完成了后台状态轮询，尚未取图；切入时立即请求，
    // 且旧播放器封面绝不短暂显示到新 TAB。
    cover_ready_ = false;
    cover_retries_left_ = 3;
    cover_retry_due_us_ = 0;
    cover_requested_.store(true);
    // 歌词同理: 旧 tab 的词立即清,新 tab 的词由 Run 循环补拉
    if (lyric_now_ != nullptr) lv_label_set_text(lyric_now_, "");
    lyr_idx_ = -1;
    lyr_requested_.store(true);
    UpdateUi();
}

void PlayerApp::HandleKey(int key) {
    ESP_LOGI(TAG, "key %d (active=%d)", key, active_ ? 1 : 0);
    if (!active_) return;
    switch (key) {
        case 0: cmd_target_.store(cur_.load()); cmd_.store(2); break;   // 音量+
        case 1: cmd_target_.store(cur_.load()); cmd_.store(3); break;   // 音量-
        case 2: cmd_target_.store(cur_.load()); cmd_.store(1); break;   // 播放/暂停
        case 3: SwitchTab(1 - cur_.load()); break;               // 双击 OK:切 tab
        case 4: cmd_target_.store(cur_.load()); cmd_.store(4); break;   // 上一曲
        case 5: cmd_target_.store(cur_.load()); cmd_.store(5); break;   // 下一曲
        default: break;
    }
}

void PlayerApp::TickProgress() {
    if (!active_ || root_ == nullptr) return;
    PlayerState& ps = st_[cur_.load()];
    static int dbg = 0;
    if (++dbg % 20 == 1) {   // 每 10 秒一条
        ESP_LOGI(TAG, "tick: valid=%d dur=%.1f pos=%.0f at=%.0f lg=%.0f/%.0f st=%s",
                 ps.valid ? 1 : 0, ps.dur_min, ps.ha_pos, ps.ha_at_epoch,
                 ps.last_good_pos, ps.last_good_time, ps.state);
    }
    if (!ps.valid || ps.dur_min <= 0.01f) return;
    // 单调时钟推进:cur = 锚定位置 + (mono_now - 锚定时刻)
    // 免疫 SNTP 未同步/时区/HA 服务器时钟偏差(实测 wall clock 方案
    // 在设备时钟偏差下 last_good 永不更新,进度条停 0)。
    double cur = ps.display_pos;
    if (strcmp(ps.state, "playing") == 0 && ps.anchor_us_ > 0) {
        cur = ps.display_pos + (double)(esp_timer_get_time() - ps.anchor_us_) / 1000000.0;
    }
    double dur_s = ps.dur_min * 60.0;
    if (dur_s > 1 && cur > dur_s) cur = dur_s;
    lv_bar_set_value(pos_bar_, (int32_t)(cur * 100 / dur_s), LV_ANIM_OFF);
    lv_label_set_text_fmt(prog_label_, "%d:%02d / %d:%02d",
                          (int)(cur / 60), (int)cur % 60,
                          (int)ps.dur_min, (int)(ps.dur_min * 60) % 60);

    // 歌词双行推进(与进度同源 cur;tab+曲名校验防串词,seek 双向都免疫
    // ——每轮从头线性扫,96 行 float 比较开销可忽略)
    if (lyric_now_ == nullptr) return;
    if (lyr_count_ > 0 && lyr_player_ == cur_.load() &&
        strcmp(lyr_key_title_, ps.title) == 0) {
        int li = -1;   // 首个时间戳前=前奏,保持空白
        if (cur >= (double)lyr_t_[0]) {
            li = 0;
            while (li + 1 < lyr_count_ && (double)lyr_t_[li + 1] <= cur) li++;
        }
        if (li != lyr_idx_) {
            lyr_idx_ = li;
            lv_label_set_text(lyric_now_,
                              li >= 0 ? (const char*)lyr_buf_ + lyr_off_[li] : "");
        }
    } else if (lyr_idx_ != -1) {
        lyr_idx_ = -1;
        lv_label_set_text(lyric_now_, "");
    }
}

void PlayerApp::UpdateUi() {
    if (root_ == nullptr) return;
    // tab 高亮
    static const PlayerCfg cfgs[kPlayers] = {
        {SONOS_HA_BASE_URL, SONOS_HA_TOKEN, SONOS_ENTITY_ID, "Sonos"},
        {POD_HA_BASE_URL, POD_HA_TOKEN, POD_ENTITY_ID, "HomePod"},
    };
    for (int i = 0; i < kPlayers; i++) {
        bool on = (i == cur_.load());
        // 纯文本 tab:选中琥珀高亮,未选中暗灰(金融 app 同款对比逻辑)
        lv_obj_set_style_text_color(tab_lbl_[i],
            on ? lv_color_hex(0xFFC24B) : lv_color_hex(0x4A545E), 0);
        lv_label_set_text(tab_lbl_[i], cfgs[i].tab_name);
    }
    PlayerState& ps = st_[cur_.load()];
    if (cover_img_ != nullptr) {
        if (cover_player_ == cur_.load() && cover_ready_) {
            lv_image_set_src(cover_img_, &cover_dsc_);
            lv_obj_clear_flag(cover_img_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(cover_img_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!ps.valid) {
        lv_label_set_text(title_label_, "连接中…");
        lv_label_set_text(artist_label_, "");
        if (lyric_now_ != nullptr) lv_label_set_text(lyric_now_, "");
        return;
    }
    const char* state_cn = strcmp(ps.state, "playing") == 0 ? "播放中"
                          : strcmp(ps.state, "paused") == 0 ? "已暂停"
                          : strcmp(ps.state, "idle") == 0 ? "空闲" : ps.state;
    char buf[80];
    snprintf(buf, sizeof(buf), "[%s]", state_cn);
    lv_label_set_text(title_label_, ps.title[0] ? ps.title : buf);
    lv_label_set_text(artist_label_, ps.artist);

}

void PlayerApp::Enter() {
    if (root_ == nullptr) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
        active_ = true;
        if (ui_timer_ != nullptr) lv_timer_resume(ui_timer_);   // 进度推进
        running_.store(true);
        if (xTaskCreate(TaskEntry, "player", 6144, this, 3, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "player task create FAILED (stack)");
            running_.store(false);
        }
        lvgl_port_unlock();
    }
}

void PlayerApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        running_.store(false);
        active_ = false;
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

void PlayerApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        cover_img_ = nullptr;
        lyric_now_ = nullptr;
        lvgl_port_unlock();
    }
}
