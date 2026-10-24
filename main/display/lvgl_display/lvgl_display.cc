#include <esp_err.h>
#include <esp_log.h>
#include <material_symbols.h>
#include <cstdlib>
#include <cstring>
#include <string>

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "dynamic_glyph_cache.h"
#include "home/finance_app.h"
#include "home/player_app.h"
#include "home/tetris_app.h"
#include "home/answer_app.h"
#include "home/radio_app.h"
#include "home/sonos_app.h"
#include "home/pod_app.h"
#include "home/timer_app.h"
#include "home/agenda_service.h"
#include "home/glm_service.h"
#include "home/poller.h"
#include "home/home_screen.h"
#include "home/weather_service.h"
#include "idle_dim.h"
#include "jpg/image_to_jpeg.h"
#include "lvgl_display.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "Display"

// 释放字形位图缓存换大块连续堆内存(缓存随运行时间增长,是碎片化
// 主源;清掉后按需自动重建)。必须在 LVGL 上下文/持锁调用。
void LvglDisplay::ReleaseCachesForMemory() {
    DisplayLockGuard lock(this);
    if (dynamic_glyph_cache_ != nullptr) {
        dynamic_glyph_cache_->Clear();
        ESP_LOGI(TAG, "glyph cache cleared for memory");
    }
}

LvglDisplay::LvglDisplay() {
    dynamic_glyph_cache_ = std::make_unique<DynamicGlyphCache>();
    // Notification timer
    esp_timer_create_args_t notification_timer_args = {
        .callback =
            [](void* arg) {
                LvglDisplay* display = static_cast<LvglDisplay*>(arg);
                DisplayLockGuard lock(display);
                lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

bool LvglDisplay::SetTextFont(std::shared_ptr<LvglFont> text_font) {
    if (text_font == nullptr || text_font->font() == nullptr) {
        return false;
    }

    DisplayLockGuard lock(this);
    auto& theme_manager = LvglThemeManager::GetInstance();
    auto light_theme = theme_manager.GetTheme("light");
    auto dark_theme = theme_manager.GetTheme("dark");
    if (light_theme == nullptr && dark_theme == nullptr) {
        return false;
    }

    // LVGL styles keep raw lv_font_t pointers. Keep the previous font owners alive until the
    // current theme has rebound every style to the new font.
    auto previous_light_font = light_theme != nullptr ? light_theme->text_font() : nullptr;
    auto previous_dark_font = dark_theme != nullptr ? dark_theme->text_font() : nullptr;
    if (light_theme != nullptr) {
        light_theme->set_text_font(text_font);
    }
    if (dark_theme != nullptr) {
        dark_theme->set_text_font(text_font);
    }
    if (current_theme_ != nullptr) {
        SetTheme(current_theme_);
    }
    previous_light_font.reset();
    previous_dark_font.reset();
    return true;
}

bool LvglDisplay::AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) {
    if (dynamic_glyph_cache_ == nullptr) {
        return false;
    }
    if (glyphs.empty()) {
        if (!TextGlyphStorageUsesPsram()) {
            ClearTextGlyphs();
        }
        return false;
    }
    if (bpp != 1 && bpp != 4) {
        return false;
    }

    DisplayLockGuard lock(this);
    if (!lock || current_theme_ == nullptr) {
        return false;
    }
    auto text_font = current_theme_->GetTextFont();
    if (text_font == nullptr) {
        return false;
    }

    auto fallback = dynamic_glyph_cache_->EnsureFont(text_font->font(), bpp);
    if (fallback == nullptr) {
        return false;
    }
    text_font->SetFallback(fallback);
    return dynamic_glyph_cache_->AddGlyphs(glyphs);
}

void LvglDisplay::ClearTextGlyphs() {
    if (dynamic_glyph_cache_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    dynamic_glyph_cache_->Clear();
}

LvglDisplay::~LvglDisplay() {
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if (low_battery_popup_ != nullptr) {
        lv_obj_del(low_battery_popup_);
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void LvglDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetStatus('%s') called before SetupUI() - message will be lost!", status);
    }
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetStatus('%s') failed: status_label_ is nullptr (SetupUI() was called but "
                     "label not created)",
                     status);
        }
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "ShowNotification('%s') called before SetupUI() - message will be lost!",
                 notification);
    }
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "ShowNotification('%s') failed: notification_label_ is nullptr (SetupUI() was "
                     "called but label not created)",
                     notification);
        }
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // Update mute icon
    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        // Update icon if mute state changes
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, MATERIAL_SYMBOLS_VOLUME_OFF);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // Update time — idle 时显示像素名片主页并刷新时钟;
    // 其他状态(听/说/激活/配网)隐藏主页,小智原生界面照常。
    //
    // 网络仲裁(本函数 ~1Hz 幂等执行):前台 app 运行时只保留它需要的网络,
    // 其余后台拉取暂停,回主页恢复——保证各 app 内存足够、网络稳定:
    //   非 idle(对话/配网) → Stop:释放 12KB 栈,让内存给 TLS/音频
    //   播放器(内存大户,自握网络) → Stop
    //   方块/计时/答案(不用网也不依赖 poller) → Pause:停掉后台 HTTP 竞争
    //   金融/邮件(数据来自 poller)/主页 → 正常运行
    {
        // PollerConfig 必须先于任何 Start():本仲裁块可能早于主页创建执行
        // Start(),而 Start() 会把配置传导给各服务;空配置一旦传导,主页创建
        // 时的 Start() 会因任务已运行而跳过,合法 URL 永远进不去。
        static bool s_poller_configured = false;
        if (!s_poller_configured) {
            PollerConfig(WEATHER_API_URL, 10, GLM_API_KEY, 5,
                         HOME_AGENDA_BASE_URL, 5, HOME_MAIL_BASE_URL, 2);
            s_poller_configured = true;
        }
        bool now_idle = (app.GetDeviceState() == kDeviceStateIdle);
        // 闲置 2 分钟自动降亮度(1Hz 推进;按键按下会立即唤醒)
        IdleDim::Get().Tick();
        bool player_on = PlayerApp::Get().Active();
        bool radio_on = RadioApp::Get().Active();   // 收音机=内存+网络大户,同播放器处理
        bool quiet_app_on = TetrisApp::Get().Active() || TimerApp::Get().Active() ||
                            AnswerApp::Get().Active();
        auto& poller = Poller::Get();
        if (!now_idle || player_on || radio_on) {
            poller.Stop();
        } else if (quiet_app_on) {
            poller.Start();   // Stop 后可能处于停止态,先确保任务在(幂等)
            poller.Pause();
        } else {
            poller.Start();   // 幂等:已运行则直接返回
            poller.Resume();
        }
    }
    if (app.GetDeviceState() == kDeviceStateIdle) {
        // 收音机播放期间跳过主页时钟/天气刷新:分钟时钟改字触发像素
        // 字形渲染(flash XIP),与音频流 DMA 缓存争抢曾致 taskLVGL
        // 卡死 WDT(整分钟规律性重启,2026-10-24 实锤);退出后下个
        // 分钟 tick 自动补刷
        if (RadioApp::Get().Active() && RadioApp::Get().TaskRunning()) {
            return;
        }
        time_t now = time(NULL);
        struct tm* tm_now = localtime(&now);
        {
            DisplayLockGuard lock(this);
            if (home_screen_ == nullptr) {
                home_screen_ = &HomeScreen::Get();
                home_screen_->Create(lv_screen_active(),
                                     static_cast<LvglTheme*>(current_theme_)->text_font()->font());
                // 其余 6 个 APP 懒创建(首次进入时),减少常驻 LVGL 内存
                // 主页就绪后启动统一轮询(天气/GLM 用量/行程提醒/金融三 tab)
                PollerConfig(WEATHER_API_URL, 10, GLM_API_KEY, 5, HOME_AGENDA_BASE_URL, 5, HOME_MAIL_BASE_URL, 2);
                Poller::Get().Start();
            }
            home_screen_->Show();
            home_screen_->RefreshClock();
        }
        if (tm_now != nullptr && tm_now->tm_year >= 2025 - 1900) {
            last_displayed_clock_min_ = tm_now->tm_hour * 60 + tm_now->tm_min;
        }
    } else {
        // Reset so the clock re-appears immediately when idle resumes.
        last_displayed_clock_min_ = -1;
        if (home_screen_ != nullptr) {
            DisplayLockGuard lock(this);
            home_screen_->Hide();
        }
    }

    esp_pm_lock_acquire(pm_lock_);
    // Update battery icon
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT;
        } else {
            const char* levels[] = {
                MATERIAL_SYMBOLS_BATTERY_ANDROID_0,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL,
            };
            int level_index = battery_level <= 0
                                  ? 0
                                  : (battery_level >= 100 ? 7 : 1 + ((battery_level - 1) * 6 / 99));
            icon = levels[level_index];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }
        if (home_screen_ != nullptr) {
            home_screen_->RefreshBattery(battery_level, charging);
        }

        // Check low battery popup only when clock tick event is triggered
        // Because when initializing, the battery level is not ready yet.
        if (low_battery_popup_ != nullptr && !update_all) {
            if (strcmp(icon, MATERIAL_SYMBOLS_BATTERY_ANDROID_0) == 0 && discharging) {
                if (lv_obj_has_flag(low_battery_popup_,
                                    LV_OBJ_FLAG_HIDDEN)) {  // Show if low battery popup is hidden
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    app.Schedule([&app]() { app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY); });
                }
            } else {
                // Hide the low battery popup when the battery is not empty
                if (!lv_obj_has_flag(low_battery_popup_,
                                     LV_OBJ_FLAG_HIDDEN)) {  // Hide if low battery popup is shown
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // Update network icon every 10 seconds
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // Don't read 4G network status during firmware upgrade to avoid occupying UART resources
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,      kDeviceStateStarting,   kDeviceStateWifiConfiguring,
            kDeviceStateListening, kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) !=
            allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

    esp_pm_lock_release(pm_lock_);
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {}

void LvglDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to take snapshot, draw_buffer is nullptr");
        return false;
    }

    // swap bytes
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);
    }

    // Clear output string and use callback version to avoid pre-allocating large memory blocks
    jpeg_data.clear();

    // Use callback-based JPEG encoder to further save memory
    bool ret =
        image_to_jpeg_cb((uint8_t*)draw_buffer->data, draw_buffer->data_size, draw_buffer->header.w,
                         draw_buffer->header.h, V4L2_PIX_FMT_RGB565, quality,
                         [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                             std::string* output = static_cast<std::string*>(arg);
                             if (data && len > 0) {
                                 output->append(static_cast<const char*>(data), len);
                             }
                             return len;
                         },
                         &jpeg_data);
    if (!ret) {
        ESP_LOGE(TAG, "Failed to convert image to JPEG");
    }

    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    ESP_LOGE(TAG, "LV_USE_SNAPSHOT is not enabled");
    return false;
#endif
}
