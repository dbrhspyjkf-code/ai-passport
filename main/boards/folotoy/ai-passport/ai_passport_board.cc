#include "wifi_board.h"
#include "display/lcd_display.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "assets/lang_config.h"
#include "cw2017_battery_monitor.h"
#include "display/home/finance_app.h"
#include "display/home/home_screen.h"
#include "display/home/tetris_app.h"
#include "display/home/answer_app.h"
#include "display/home/player_app.h"
#include "display/home/timer_app.h"
#include "display/home/mail_app.h"
#include "display/idle_dim.h"
#include "esp_lvgl_port.h"
#include "ssid_manager.h"
#if __has_include("wifi_defaults.h")
#include "wifi_defaults.h"  // 私有默认凭证(gitignored);缺失时下方的默认空定义生效
#else
#define HOME_DEFAULT_SSID_1 ""
#define HOME_DEFAULT_PASSWORD_1 ""
#define HOME_DEFAULT_SSID_2 ""
#define HOME_DEFAULT_PASSWORD_2 ""
#endif

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <button_adc.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "AiPassport"

int64_t g_ok_longpress_us = 0;   // OK 长按时刻(防 click 双触发)

// Physical keys share one ADC pin through a resistor ladder (see config.h).
enum {
    kAdcButtonUp = 0,
    kAdcButtonDown,
    kAdcButtonOk,
    kAdcButtonNum,
};

class AiPassportBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button* adc_button_[kAdcButtonNum];
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    LcdDisplay* display_;
    Cw2017BatteryMonitor* battery_;

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // CW2017 fuel gauge is optional; a missing chip just disables battery UI.
        battery_ = new Cw2017BatteryMonitor(codec_i2c_bus_, BATTERY_CW2017_ADDR);
        battery_->Initialize();
    }

    void ChangeVolume(int delta) {
        auto codec = GetAudioCodec();
        auto volume = codec->output_volume() + delta;
        if (volume > 100) {
            volume = 100;
        }
        if (volume < 0) {
            volume = 0;
        }
        codec->SetOutputVolume(volume);
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void ToggleChat() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateStarting) {
            EnterWifiConfigMode();
            return;
        }
        app.ToggleChatState();
    }

    void InitializeButtons() {
        for (int i = 0; i < kAdcButtonNum; i++) {
            adc_button_[i] = nullptr;
        }
        adc_oneshot_unit_init_cfg_t init_cfg = {
            .unit_id = ADC_UNIT_1,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle_));

        button_adc_config_t adc_cfg = {};
        adc_cfg.adc_handle = &adc_handle_;
        adc_cfg.unit_id = ADC_UNIT_1;
        adc_cfg.adc_channel = ADC_CHANNEL_0;

        adc_cfg.button_index = kAdcButtonUp;
        adc_cfg.min = BSP_ADC_BUTTON_UP_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_UP_MAX;
        adc_button_[kAdcButtonUp] = new AdcButton(adc_cfg);

        adc_cfg.button_index = kAdcButtonDown;
        adc_cfg.min = BSP_ADC_BUTTON_DOWN_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_DOWN_MAX;
        adc_button_[kAdcButtonDown] = new AdcButton(adc_cfg);

        adc_cfg.button_index = kAdcButtonOk;
        adc_cfg.min = BSP_ADC_BUTTON_OK_MIN;
        adc_cfg.max = BSP_ADC_BUTTON_OK_MAX;
        adc_button_[kAdcButtonOk] = new AdcButton(adc_cfg);

        auto& finance = FinanceApp::Get();
        auto up = adc_button_[kAdcButtonUp];
        auto down = adc_button_[kAdcButtonDown];
        auto ok = adc_button_[kAdcButtonOk];

        // 任意按键按下即视为用户交互:立即从闲置降亮度状态恢复。
        auto wake_display = []() { IdleDim::Get().Notify(); };
        up->OnPressDown(wake_display);
        down->OnPressDown(wake_display);
        ok->OnPressDown(wake_display);

        // ============ UP 键 ============
        // 短按: APP 内功能 / 主页=launcher
        up->OnClick([this, &finance]() {
            Application::GetInstance().Schedule([this, &finance]() {
                if (finance.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        finance.HandleKey(FinanceApp::KEY_UP);
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& tetris = TetrisApp::Get();
                if (tetris.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        tetris.HandleKey(0);   // 左移
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& player = PlayerApp::Get();
                if (player.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        player.HandleKey(0);   // 音量+
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& mailU = MailApp::Get();
                if (mailU.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        mailU.HandleKey(0);   // 上一封
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& timerU = TimerApp::Get();
                if (timerU.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        timerU.HandleKey(0);   // 上一个预设
                        lvgl_port_unlock();
                    }
                    return;
                }
                if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        HomeScreen::Get().HandleLauncherKey(0);
                        lvgl_port_unlock();
                    }
                    return;
                }
                ChangeVolume(10);
            });
        });
        // 长按 UP: 播放器=上一曲;金融=切tab;主页=音量100
        up->OnLongPress([this, &finance]() {
            Application::GetInstance().Schedule([this, &finance]() {
                auto& player = PlayerApp::Get();
                if (player.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        player.HandleKey(4);   // 上一曲
                        lvgl_port_unlock();
                    }
                    return;
                }
                if (finance.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        finance.HandleKey(FinanceApp::KEY_UP_LONG);
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& tetris = TetrisApp::Get();
                if (tetris.Active()) {
                    tetris.Leave();
                    return;
                }
                GetAudioCodec()->SetOutputVolume(100);
                GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
            });
        });

        // ============ DOWN 键 ============
        down->OnClick([this, &finance]() {
            Application::GetInstance().Schedule([this, &finance]() {
                if (finance.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        finance.HandleKey(FinanceApp::KEY_DOWN);
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& tetris = TetrisApp::Get();
                if (tetris.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        tetris.HandleKey(1);   // 右移
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& player = PlayerApp::Get();
                if (player.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        player.HandleKey(1);   // 音量-
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& mailD = MailApp::Get();
                if (mailD.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        mailD.HandleKey(1);   // 下一封
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& timerD = TimerApp::Get();
                if (timerD.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        timerD.HandleKey(1);   // 下一个预设
                        lvgl_port_unlock();
                    }
                    return;
                }
                if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        HomeScreen::Get().HandleLauncherKey(1);
                        lvgl_port_unlock();
                    }
                    return;
                }
                ChangeVolume(-10);
            });
        });
        // 长按 DOWN: 播放器=下一曲;金融=切tab;方块=硬降;主页=静音
        down->OnLongPress([this, &finance]() {
            Application::GetInstance().Schedule([this, &finance]() {
                auto& player = PlayerApp::Get();
                if (player.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        player.HandleKey(5);   // 下一曲
                        lvgl_port_unlock();
                    }
                    return;
                }
                if (finance.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        finance.HandleKey(FinanceApp::KEY_DOWN_LONG);
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& tetris = TetrisApp::Get();
                if (tetris.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        tetris.HandleKey(5);   // 硬降
                        lvgl_port_unlock();
                    }
                    return;
                }
                GetAudioCodec()->SetOutputVolume(0);
                GetDisplay()->ShowNotification(Lang::Strings::MUTED);
            });
        });

        // ============ OK 键 ============
        ok->OnClick([this, &finance]() {
            ESP_LOGI(TAG, "[key] OK click @%lld", (long long)esp_timer_get_time());
            Application::GetInstance().Schedule([this, &finance]() {
                extern int64_t g_ok_longpress_us;
                if (g_ok_longpress_us != 0 &&
                    esp_timer_get_time() - g_ok_longpress_us < 3000000LL) {
                    return;   // 长按后 3s 内的 click 忽略(ADC 抖动防双触发)
                }
                if (finance.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        finance.HandleKey(FinanceApp::KEY_OK);
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& tetris = TetrisApp::Get();
                if (tetris.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        tetris.HandleKey(2);   // 旋转
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& player = PlayerApp::Get();
                if (player.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        player.HandleKey(2);   // 播放/暂停
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& mailOK = MailApp::Get();
                if (mailOK.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        mailOK.HandleKey(2);   // 刷新
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& timerOK = TimerApp::Get();
                if (timerOK.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        timerOK.HandleKey(2);   // 开始/暂停/继续
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& answerOK = AnswerApp::Get();
                if (answerOK.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        answerOK.HandleKey(2);   // 翻开答案
                        lvgl_port_unlock();
                    }
                    return;
                }
                // 主页:对话中/刚结束=切换对话;平静=进选中 APP
                {
                    static int64_t s_last_chat_us = 0;
                    int64_t now_us = esp_timer_get_time();
                    bool chat_recent =
                        Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
                        (now_us - s_last_chat_us < 5000000LL && s_last_chat_us != 0);
                    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle && !chat_recent) {
                        if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                            HomeScreen::Get().HandleLauncherKey(2);
                            lvgl_port_unlock();
                        }
                    } else {
                        s_last_chat_us = now_us;
                        ToggleChat();
                    }
                }
            });
        });
        // 双击 OK: 播放器=切 tab
        ok->OnDoubleClick([this]() {
            Application::GetInstance().Schedule([this]() {
                auto& player = PlayerApp::Get();
                if (player.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        player.HandleKey(3);
                        lvgl_port_unlock();
                    }
                }
            });
        });
        // 长按 OK: APP=退出;主页=对话
        ok->OnLongPress([this, &finance]() {
            ESP_LOGI(TAG, "[key] OK long @%lld", (long long)esp_timer_get_time());
            Application::GetInstance().Schedule([this, &finance]() {
                extern int64_t g_ok_longpress_us;
                g_ok_longpress_us = esp_timer_get_time();
                if (finance.Active()) {
                    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
                        finance.HandleKey(FinanceApp::KEY_OK_LONG);
                        lvgl_port_unlock();
                    }
                    return;
                }
                auto& tetris = TetrisApp::Get();
                if (tetris.Active()) { tetris.Leave(); return; }
                auto& player = PlayerApp::Get();
                if (player.Active()) { player.Leave(); return; }
                auto& mailL = MailApp::Get();
                if (mailL.Active()) { mailL.Leave(); return; }
                auto& timerL = TimerApp::Get();
                if (timerL.Active()) { timerL.Leave(); return; }
                auto& answerL = AnswerApp::Get();
                if (answerL.Active()) { answerL.Leave(); return; }
                if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    ToggleChat();
                }
            });
        });
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // NVS 无任何已保存 SSID 时,注入家庭默认网络(详见 wifi_defaults.h);
    // 注入后仍连不上,小智会照常进入配网模式。
    void InjectDefaultWifi() {
        auto& ssid_manager = SsidManager::GetInstance();
        if (!ssid_manager.GetSsidList().empty()) return;
        if (sizeof(HOME_DEFAULT_SSID_1) > 1) {
            ssid_manager.AddSsid(HOME_DEFAULT_SSID_1, HOME_DEFAULT_PASSWORD_1);
            ESP_LOGI(TAG, "注入默认 Wi-Fi: %s", HOME_DEFAULT_SSID_1);
        }
        if (sizeof(HOME_DEFAULT_SSID_2) > 1) {
            ssid_manager.AddSsid(HOME_DEFAULT_SSID_2, HOME_DEFAULT_PASSWORD_2);
            ESP_LOGI(TAG, "注入默认 Wi-Fi: %s", HOME_DEFAULT_SSID_2);
        }
        if (sizeof(HOME_DEFAULT_SSID_3) > 1) {
            ssid_manager.AddSsid(HOME_DEFAULT_SSID_3, HOME_DEFAULT_PASSWORD_3);
            ESP_LOGI(TAG, "注入默认 Wi-Fi: %s", HOME_DEFAULT_SSID_3);
        }
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;  // -1 -> software reset
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);

        // Panel-specific power/porch/gamma sequence for the ST7789P3 module
        // used on the Passport (copied from the original badge firmware).
        static const struct {
            uint8_t command;
            uint8_t data[16];
            uint8_t data_length;
            uint16_t delay_ms;
        } kSt7789P3InitCommands[] = {
            {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},
            {0xB7, {0x35}, 1, 0},
            {0xBB, {0x21}, 1, 0},
            {0xC0, {0x2C}, 1, 0},
            {0xC2, {0x01}, 1, 0},
            {0xC3, {0x0B}, 1, 0},
            {0xC4, {0x20}, 1, 0},
            {0xC6, {0x0F}, 1, 0},
            {0xD0, {0xA7, 0xA1}, 2, 0},
            {0xD0, {0xA4, 0xA1}, 2, 0},
            {0xD6, {0xA1}, 1, 0},
            {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
                    0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0},
            {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
                    0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10},
        };
        for (const auto& cmd : kSt7789P3InitCommands) {
            esp_lcd_panel_io_tx_param(panel_io, cmd.command, cmd.data, cmd.data_length);
            if (cmd.delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(cmd.delay_ms));
            }
        }

        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_set_gap(panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

public:
    AiPassportBoard() : display_(nullptr), battery_(nullptr) {
        InitializeCodecI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
        InjectDefaultWifi();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!battery_ || !battery_->IsPresent()) {
            return false;
        }
        int soc = battery_->GetBatteryLevel();
        if (soc < 0) {
            return false;
        }
        level = soc;
        // CW2017 reports no charge state and the Passport has no charge-detect
        // GPIO, so report a plain (discharging) reading.
        charging = false;
        discharging = true;
        return true;
    }
};

DECLARE_BOARD(AiPassportBoard);
