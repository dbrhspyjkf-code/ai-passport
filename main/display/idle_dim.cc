#include "idle_dim.h"

#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "board.h"

static const char* TAG = "IdleDim";

namespace {
// 闲置多久后降亮度(毫秒)
constexpr uint32_t kIdleTimeoutMs = 2 * 60 * 1000;
// 降到的亮度(0-100)。只是变暗,不熄灭,避免误以为关机。
constexpr uint8_t kDimBrightness = 15;
// 读不到历史亮度时的兜底值(与 Settings 默认一致)
constexpr uint8_t kFallbackBrightness = 75;
}  // namespace

IdleDim& IdleDim::Get() {
    static IdleDim instance;
    return instance;
}

IdleDim::IdleDim() : last_activity_ms_(NowMs()) {}

uint32_t IdleDim::NowMs() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void IdleDim::Notify() {
    last_activity_ms_.store(NowMs());
    if (!dimmed_.exchange(false)) return;

    // 恢复:无条件回到降亮度前的值。即使渐变尚未完成也立即反向,
    // 避免"按键后仍停在暗屏"的情况。
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) return;
    uint8_t target = saved_brightness_.load();
    if (target <= kDimBrightness) target = kFallbackBrightness;
    ESP_LOGI(TAG, "activity: restore brightness %u", target);
    backlight->SetBrightness(target, false);
}

void IdleDim::Tick() {
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) return;

    // 对话/配网/播报期间一律视为活跃:既不计入闲置,也顺带从暗屏恢复。
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        Notify();
        return;
    }

    const uint32_t idle_ms = NowMs() - last_activity_ms_.load();
    if (dimmed_.load() || idle_ms < kIdleTimeoutMs) return;

    const uint8_t current = backlight->brightness();
    if (current <= kDimBrightness) {
        // 已经是暗的(例如用户手动调低),不必再记一次"恢复目标"
        dimmed_.store(true);
        saved_brightness_.store(current);
        return;
    }
    saved_brightness_.store(current);
    dimmed_.store(true);
    ESP_LOGI(TAG, "idle %us: dim brightness %u -> %u",
             (unsigned)(idle_ms / 1000), current, kDimBrightness);
    backlight->SetBrightness(kDimBrightness, false);
}
