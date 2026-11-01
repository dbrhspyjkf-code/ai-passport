// health_app —— 健康数据查看器实现(2 列彩色边框卡片网格)。
// 数据来自 HealthService(hermes /api/health,与 M5Tab5 app_health 同源)。

#include "theme.h"
#include "health_app.h"

#include <cstdio>
#include <cstring>

#include "esp_lvgl_port.h"

#include "health_service.h"

LV_FONT_DECLARE(font_home_cn_16);

HealthApp& HealthApp::Get() {
    static HealthApp inst;
    return inst;
}

static const uint32_t C_TEXT = theme::fg(), C_DIM = theme::dim();

// 卡片边框色(Tab5 app_health 同款 iOS 色板,按 HealthService.metrics 顺序)
static const uint32_t kCardColors[HealthService::kMetricCount] = {
    0xFF9500,   // 步数 橙
    0x34C759,   // 距离 绿
    0xFF9F0A,   // 活动 琥珀
    0xFF3B30,   // 心率 红
    0xFF2D55,   // 静息 粉
    0x30B0C7,   // 变异 青
    0x007AFF,   // 血氧 蓝
    0x5AC8FA,   // 呼吸 天蓝
    0x5856D6,   // 爬楼 紫
    0x5E5CE6,   // 睡眠 靛
};

void HealthApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(theme::bg()), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 8, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 页头:标题(左,亮) + 数据时刻(右,灰)
    title_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(title_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(title_label_, "健康");
    lv_obj_align(title_label_, LV_ALIGN_TOP_LEFT, 0, 2);

    updated_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(updated_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(updated_label_, lv_color_hex(C_DIM), 0);
    lv_label_set_text(updated_label_, "");
    lv_obj_align(updated_label_, LV_ALIGN_TOP_RIGHT, 0, 2);

    // 2×5 卡片网格(固定尺寸,只改文本内容)
    for (int r = 0; r < kRows; r++) {
        for (int c = 0; c < kCols; c++) {
            int idx = r * kCols + c;
            int x = c * (kCardW + 8);
            int y = 28 + r * (kCardH + 4);

            lv_obj_t* card = lv_obj_create(root_);
            lv_obj_set_size(card, kCardW, kCardH);
            lv_obj_set_style_bg_color(card, lv_color_hex(theme::hint()), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(card,
                lv_color_hex(kCardColors[idx % HealthService::kMetricCount]), 0);
            lv_obj_set_style_border_width(card, 1, 0);
            lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
            lv_obj_set_style_radius(card, 4, 0);
            lv_obj_set_style_pad_all(card, 3, 0);
            lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(card, x, y);
            cards_[r][c] = card;

            name_lbl_[r][c] = lv_label_create(card);
            lv_obj_set_style_text_font(name_lbl_[r][c], &font_home_cn_16, 0);
            lv_obj_set_style_text_color(name_lbl_[r][c], lv_color_hex(C_DIM), 0);
            lv_label_set_text(name_lbl_[r][c], "");
            lv_obj_align(name_lbl_[r][c], LV_ALIGN_TOP_LEFT, 2, 0);

            val_lbl_[r][c] = lv_label_create(card);
            lv_obj_set_style_text_font(val_lbl_[r][c], &font_home_cn_16, 0);
            lv_obj_set_style_text_color(val_lbl_[r][c], lv_color_hex(C_TEXT), 0);
            lv_label_set_text(val_lbl_[r][c], "");
            lv_obj_align(val_lbl_[r][c], LV_ALIGN_BOTTOM_LEFT, 2, 0);
        }
    }

    // 底部提示
    lv_obj_t* hint = lv_label_create(root_);
    lv_obj_set_style_text_font(hint, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_label_set_text(hint, "OK 刷新 · 长按退出");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    // 居中提示(错误/获取中)
    state_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(state_label_, &font_home_cn_16, 0);
    lv_obj_set_style_text_color(state_label_, lv_color_hex(C_DIM), 0);
    lv_label_set_text(state_label_, "");
    lv_obj_align(state_label_, LV_ALIGN_TOP_MID, 0, 160);

    sync_timer_ = lv_timer_create([](lv_timer_t* t) {
        static_cast<HealthApp*>(lv_timer_get_user_data(t))->SyncFromCache();
    }, 500, this);
    lv_timer_pause(sync_timer_);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void HealthApp::Render() {
    if (root_ == nullptr) return;
    auto& hs = HealthService::Get();
    if (!hs.Valid()) {
        lv_label_set_text(updated_label_, "");
        for (int r = 0; r < kRows; r++)
            for (int c = 0; c < kCols; c++) {
                lv_label_set_text(name_lbl_[r][c], "");
                lv_label_set_text(val_lbl_[r][c], "");
            }
        lv_label_set_text(state_label_, "暂无数据");
        return;
    }
    lv_label_set_text(state_label_, "");
    lv_label_set_text(updated_label_, hs.UpdatedAt());
    // 数值一律 snprintf(%f 陷阱);缺数据的卡只写名称不写值
    for (int i = 0; i < HealthService::kMetricCount; i++) {
        const HealthService::Metric& m = hs.metrics()[i];
        int r = i / kCols, c = i % kCols;
        if (r >= kRows) break;
        lv_label_set_text(name_lbl_[r][c], m.name);
        char buf[24];
        if (!m.present) {
            buf[0] = '\0';
        } else if (strcmp(m.type, "sleep_analysis") == 0) {
            int h = (int)(m.value / 60);
            int mm = (int)(m.value + 0.5f) % 60;
            snprintf(buf, sizeof(buf), "%d:%02d", h, mm);
        } else if (m.value >= 100) {
            snprintf(buf, sizeof(buf), "%d %s", (int)(m.value + 0.5f), m.unit);
        } else {
            snprintf(buf, sizeof(buf), "%.1f %s", (double)m.value, m.unit);
        }
        lv_label_set_text(val_lbl_[r][c], buf);
    }
}

void HealthApp::SyncFromCache() {
    if (!active_) return;
    const uint32_t version = HealthService::Get().Version();
    if (version == seen_version_) return;
    seen_version_ = version;
    Render();
}

void HealthApp::HandleKey(int key) {
    if (!active_) return;
    switch (key) {
        case 0:
        case 1:
            break;
        case 2:
            HealthService::Get().RequestNow();
            lv_label_set_text(state_label_, "获取中…");
            break;
        default:
            break;
    }
}

void HealthApp::Enter() {
    if (root_ == nullptr) return;
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
        active_ = true;
        seen_version_ = HealthService::Get().Version();
        Render();
        HealthService::Get().RequestNow();
        if (!HealthService::Get().Valid()) lv_label_set_text(state_label_, "获取中…");
        if (sync_timer_ != nullptr) lv_timer_resume(sync_timer_);
        lvgl_port_unlock();
    }
}

void HealthApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        active_ = false;
        if (sync_timer_ != nullptr) lv_timer_pause(sync_timer_);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

void HealthApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        if (sync_timer_ != nullptr) {
            lv_timer_delete(sync_timer_);
            sync_timer_ = nullptr;
        }
        lvgl_port_unlock();
    }
}
