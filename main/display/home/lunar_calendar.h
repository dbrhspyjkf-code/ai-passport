#pragma once

// lunar_calendar —— 公历转农历(1900-2100,经典表驱动算法)。
// 表数据为社区公用的农历信息常数(每年 4 字节:13 个月大小月位 + 闰月号)。

#include <cstddef>

// 把公历日期转为农历日期中文字符串,如 "八月廿六"、"闰四月 初五"。
// 超出支持范围(1900-2100)返回 false。
bool SolarToLunarCn(int year, int month, int day, char* out, size_t cap);
