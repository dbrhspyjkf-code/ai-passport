#pragma once
// LVGL 断言(LV_ASSERT_MALLOC / NULL / STYLE 失败)的处理动作。
//
// LVGL 默认 LV_ASSERT_HANDLER 是 `while(1);`:断言所在任务原地自旋,IDLE 饿死,
// 要等 task_wdt 10s 后才重启,且现场只剩 WDT 的寄存器快照——2026-10-25 一天内
// Flappy 进游戏 / 收音机 14 台列表 / 慢开机建主页 三次撞上同一机制(堆碎片下
// malloc 返回 NULL)。这里改为立即触发非法指令 panic:带完整回溯的即时重启,
// 失败原因一眼可见。
//
// 注入方式:CONFIG_LV_ASSERT_HANDLER_INCLUDE="lv_assert_handler.h"(LVGL 在
// lv_assert.h 里 include 本文件,此时 lv_conf_internal 已给出默认值,故先 undef)。
#include <stdint.h>

#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER __builtin_trap();
