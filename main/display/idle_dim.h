#pragma once

// idle_dim —— 闲置自动降亮度。
//
// 规则:设备处于 idle(无对话/配网)且连续 2 分钟无任何按键交互时,
// 背光平滑降到 kDimBrightness;任意按键按下立即恢复原亮度。
//
// 亮度只做运行期临时调整(permanent=false),不写 Settings,
// 因此不会覆盖用户在 MCP/配网里设置的永久亮度。

#include <atomic>
#include <cstdint>

class IdleDim {
public:
    static IdleDim& Get();

    // 记录一次用户交互(按键按下)。若当前处于降亮度状态,立即恢复。
    void Notify();

    // 1Hz 调用(UpdateStatusBar 内):推进闲置计时,到点降亮度。
    void Tick();

private:
    IdleDim();

    static uint32_t NowMs();

    std::atomic<uint32_t> last_activity_ms_;
    std::atomic<bool> dimmed_{false};
    std::atomic<uint8_t> saved_brightness_{0};
};
