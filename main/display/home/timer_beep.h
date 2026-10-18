#pragma once

// TimerApp 到时提示音的线程桥接：只把请求转交给 Application 主任务。
// 具体音频生命周期由 Application/AudioService/AudioOutputTask 管理。
#include "application.h"

inline void StartTimerBeepTask() {
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().PlayTimerSound();
    });
}
