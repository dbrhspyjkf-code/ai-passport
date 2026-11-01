#pragma once

/*
 * theme — 全局配色主题（2026-11-01 起，参考 M5Tab5 theme 系统）。
 *
 * 所有 app 的界面颜色统一从这里取，不再各写死 hex。
 * 新增主题只改本文件 kThemes 表。
 *
 * - 当前主题索引持久化在 NVS("ui"/"theme")，惰性读取；
 * - 主页长按 UP 一键切换，切换后主页立即重建，
 *   其他 app 下次打开时生效（各自 Create/Enter 时取值）；
 * - 每文件只需把 #define C_BG 0x0a1f14 改为 #define C_BG theme::bg()。
 */

#include <cstdint>
#include "settings.h"

namespace theme {

struct Palette {
    const char* name;
    uint32_t bg;     // 深底
    uint32_t fg;     // 主前景(文字/线条)
    uint32_t dim;    // 次级(标签/弱文字)
    uint32_t hint;   // 更暗(按键提示/框线)
};

inline const Palette kThemes[] = {
    {"荧绿", 0x0a1f14, 0xb7f5c4, 0x4f7a5e, 0x34573f},   // 原版深墨绿
    {"暖橙", 0x1c1006, 0xf5cf9e, 0x8a6b42, 0x5c4526},   // 深棕暖橙
    {"海蓝", 0x081522, 0x9ecbe8, 0x4a6d8a, 0x2e4a5e},   // 深海蓝
    {"岩紫", 0x140d1e, 0xc4a8e8, 0x6b5488, 0x453663},   // 深紫
};
inline const int kCount = (int)(sizeof(kThemes) / sizeof(kThemes[0]));

namespace detail {
inline int g_idx = -1;   // -1 = 尚未从 NVS 读取
}

inline int cur_index() {
    if (detail::g_idx < 0) {
        Settings st("ui", false);
        detail::g_idx = st.GetInt("theme", 0);
        if (detail::g_idx < 0 || detail::g_idx >= kCount) detail::g_idx = 0;
    }
    return detail::g_idx;
}

// 色值入口(各 app 的 #define 调用这些)
inline uint32_t bg()   { return kThemes[cur_index()].bg; }
inline uint32_t fg()   { return kThemes[cur_index()].fg; }
inline uint32_t dim()  { return kThemes[cur_index()].dim; }
inline uint32_t hint() { return kThemes[cur_index()].hint; }

// 循环切到下一个主题，持久化，返回新主题名
inline const char* cycle() {
    detail::g_idx = (cur_index() + 1) % kCount;
    Settings st("ui", true);
    st.SetInt("theme", detail::g_idx);
    return kThemes[detail::g_idx].name;
}

}  // namespace theme
