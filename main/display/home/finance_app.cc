// finance_app.cc —— 金融 APP 实现。
// 后台 task 顺序拉取三个端点(每 tab 一次 HTTP),完成后 UI 持锁重建列表。
#include "finance_app.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "poller.h"
#include "agenda_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "finance";

// 配色与主页一致
#define C_BG 0x0a1f14
#define C_FG 0xb7f5c4
#define C_DIM 0x4f7a5e
#define C_HINT 0x34573f
#define C_UP 0xe5493f     // 涨-红
#define C_DOWN 0x2fa653   // 跌-绿

static const char* kBaseUrls[3] = {
    "http://192.168.1.200:8766/api/stocks/indices",
    "http://192.168.1.200:8766/api/stocks/portfolio",
    "http://192.168.1.200:8766/api/market/overview",
};
static const char* kTabNames[3] = {"指数", "自选", "市场"};

// 全量中文字体(GB2312+ASCII,1bpp 16px):金融列表/任意动态中文专用
LV_FONT_DECLARE(font_home_cn_16);

FinanceApp& FinanceApp::Get() {
    static FinanceApp instance;
    return instance;
}

void FinanceApp::RefreshTab(int tab) {
    if (root_ == nullptr) return;   // APP 已销毁,Poller 调用直接跳过
    static const bool kPortfolioStyle[3] = {false, true, false};
    std::vector<Item> out;
    FetchJsonArray(kBaseUrls[tab], &out, kPortfolioStyle[tab]);
    bool ok = !out.empty();
    ESP_LOGI(TAG, "tab %d(%s): %s", tab, kTabNames[tab], ok ? "OK" : "失败");
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (ok) {
            data_[tab] = out;
            tab_failed_[tab] = false;
        } else {
            tab_failed_[tab] = true;
        }
        // 数据到达即重建(不要求 APP 打开):进入时看到的总是最新内容
        if (current_tab_ == tab) RebuildList();
        lvgl_port_unlock();
    }
}

bool FinanceApp::FetchJsonArray(const char* url, std::vector<Item>* out, bool portfolio_style) {
    static char body[8192];
    int len = 0;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 2048;
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
    if (!ok) return false;

    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) return false;
    out->clear();
    cJSON* arr = portfolio_style ? cJSON_GetObjectItem(root, "items")
                                 : cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(arr)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, arr) {
            Item item;
            auto str_field = [&](const char* k) -> const char* {
                cJSON* v = cJSON_GetObjectItem(it, k);
                return cJSON_IsString(v) && v->valuestring ? v->valuestring : nullptr;
            };
            auto num_field = [&](const char* k) -> double {
                cJSON* v = cJSON_GetObjectItem(it, k);
                return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
            };
            if (portfolio_style) {
                const char* name = str_field("name");
                if (name == nullptr) continue;
                item.name = name;
                char price[24];
                snprintf(price, sizeof(price), "%.2f", num_field("price"));
                item.value = price;
                item.pct = (float)num_field("chg");
            } else {
                const char* name = str_field("name");
                if (name == nullptr) continue;
                item.name = name;
                const char* val = str_field("price");
                if (val == nullptr) val = str_field("value");
                item.value = val ? val : "";
                item.pct = (float)num_field("pct");
                if (item.pct == 0.0f) item.pct = (float)num_field("chg");
            }
            out->push_back(item);
        }
    }
    cJSON_Delete(root);
    return true;
}

void FinanceApp::Create(lv_obj_t* parent, const lv_font_t* list_font) {
    if (root_ != nullptr) return;
    // 忽略传入的主题字体(子集缺字);金融数据是动态中文,用全量字体
    (void)list_font;
    list_font_ = &font_home_cn_16;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 标题 + Tab 标签(当前高亮)
    for (int i = 0; i < 3; i++) {
        tab_labels_[i] = lv_label_create(root_);
        lv_obj_set_style_text_font(tab_labels_[i], list_font_, 0);
        lv_obj_set_style_text_color(tab_labels_[i], lv_color_hex(C_DIM), 0);
        lv_label_set_text(tab_labels_[i], kTabNames[i]);
        lv_obj_align(tab_labels_[i], LV_ALIGN_TOP_MID, (i - 1) * 72, 8);
    }

    // 当前 Tab 高亮指示条
    // (简化:选中的 tab 文字用前景色,未选中用暗色;SelectTab 里切换颜色)

    // 列表容器(可滚动)
    list_box_ = lv_obj_create(root_);
    lv_obj_set_size(list_box_, LV_HOR_RES, LV_VER_RES - 44 - 24);
    lv_obj_set_style_bg_color(list_box_, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(list_box_, 0, 0);
    lv_obj_set_style_pad_all(list_box_, 4, 0);
    lv_obj_set_flex_flow(list_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_box_, 6, 0);
    lv_obj_set_scrollbar_mode(list_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(list_box_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_align(list_box_, LV_ALIGN_TOP_MID, 0, 44);

    // 底部提示
    lv_obj_t* hint = lv_label_create(root_);
    lv_obj_set_style_text_font(hint, list_font_, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_HINT), 0);
    lv_label_set_text(hint, "上下 滚动 · 长按 切页 · OK 刷新");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    SelectTab(0);
}

void FinanceApp::SelectTab(int idx) {
    if (idx < 0 || idx > 2) return;
    current_tab_ = idx;
    if (root_ == nullptr) return;
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_text_color(tab_labels_[i],
            lv_color_hex(i == idx ? C_FG : C_HINT), 0);
    }
    RebuildList();
}

void FinanceApp::RebuildList() {
    if (list_box_ == nullptr) return;
    ESP_LOGI(TAG, "rebuild: tab=%d items=%d", current_tab_,
             (int)data_[current_tab_].size());
    lv_obj_clean(list_box_);
    auto& items = data_[current_tab_];
    if (items.empty()) {
        lv_obj_t* empty = lv_label_create(list_box_);
        lv_obj_set_style_text_font(empty, list_font_, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(C_DIM), 0);
        lv_label_set_text(empty, tab_failed_[current_tab_] ? "获取失败 · OK 重试" : "加载中…");
        return;
    }
    for (auto& it : items) {
        // 双行卡片(flex 排版):第一行名称;第二行价格+涨跌
        lv_obj_t* row = lv_obj_create(list_box_);
        lv_obj_set_size(row, LV_HOR_RES - 16, 24);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(C_HINT), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* name = lv_label_create(row);
        lv_obj_set_style_text_font(name, list_font_, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_FG), 0);
        lv_label_set_text(name, it.name.c_str());
        lv_obj_set_width(name, 104);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* val = lv_label_create(row);
        lv_obj_set_style_text_font(val, list_font_, 0);
        lv_obj_set_style_text_color(val, lv_color_hex(C_FG), 0);
        lv_label_set_text(val, it.value.c_str());
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -64, 0);

        char pctbuf[16];
        snprintf(pctbuf, sizeof(pctbuf), "%+.2f%%", it.pct);
        lv_obj_t* pct = lv_label_create(row);
        lv_obj_set_style_text_font(pct, list_font_, 0);
        lv_obj_set_style_text_color(pct, lv_color_hex(
            it.pct > 0 ? C_UP : (it.pct < 0 ? C_DOWN : C_DIM)), 0);
        lv_label_set_text(pct, pctbuf);
        lv_obj_align(pct, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

void FinanceApp::Enter() {
    // LVGL 操作必须持锁;带超时——拿不到锁放弃本次(绝不卡 main task)
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && !active_) {
            lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
            active_ = true;
            ESP_LOGI(TAG, "enter: tab=%d items=%d", current_tab_,
                     (int)data_[current_tab_].size());
            RebuildList();  // 立即展示缓存数据(可能为空/失败态)
        }
        lvgl_port_unlock();
    }
    // 数据由统一轮询 task 常驻维护,进入即显示缓存并请求立即刷新
    Poller::Get().RequestRefresh();
}

void FinanceApp::Destroy() {
    Leave();   // active_=false + 隐藏
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);   // 递归删子对象
            root_ = nullptr;
        }
        lvgl_port_unlock();
    }
}

void FinanceApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && active_) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            active_ = false;
        }
        lvgl_port_unlock();
    }
    // 数据留在内存,再次进入立即可见;后台 task 常驻轮询但频率低
}

void FinanceApp::HandleKey(Key key) {
    // 调用约定:持 LVGL 锁的调用方才允许进入(board 侧已包锁)
    if (!active_ || root_ == nullptr) return;
    switch (key) {
        case KEY_UP: {
            // 轻按:向上滚动(按剩余可滚量钳位,不越界)
            int32_t top = lv_obj_get_scroll_top(list_box_);
            int32_t dy = top < 72 ? top : 72;
            lv_obj_scroll_by(list_box_, 0, dy, LV_ANIM_ON);
            break;
        }
        case KEY_DOWN: {
            // 轻按:向下滚动(按剩余可滚量钳位,不越界)
            int32_t bottom = lv_obj_get_scroll_bottom(list_box_);
            int32_t dy = bottom < 72 ? bottom : 72;
            lv_obj_scroll_by(list_box_, 0, -dy, LV_ANIM_ON);
            break;
        }
        case KEY_OK:
            // 手动刷新:清当前页进入加载态并唤醒轮询 task
            tab_failed_[0] = tab_failed_[1] = tab_failed_[2] = false;
            data_[current_tab_].clear();
            RebuildList();
            Poller::Get().RequestRefresh();
            break;
        case KEY_OK_LONG:
            Leave();
            break;
        case KEY_UP_LONG:
            SelectTab((current_tab_ + 2) % 3);
            break;
        case KEY_DOWN_LONG:
            SelectTab((current_tab_ + 1) % 3);
            break;
    }
}


