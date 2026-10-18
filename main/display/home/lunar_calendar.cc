// lunar_calendar.cc —— 公历转农历(基准:1900-01-31 = 农历 1900 年正月初一)。
// 信息表由 lunardate(PyPI)逐月天数推导生成,并与经典表(0x04bd8 开头)逐项核对;
// 表项:bit16 闰月大小;bit15-4 普通月 1-12 大小(1=大月 30 天);bit3-0 闰月号。
// 覆盖 1900-2099。
#include "lunar_calendar.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

const uint32_t kLunarInfo[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0,
    0x09ad0, 0x055d2, 0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540,
    0x0d6a0, 0x0ada2, 0x095b0, 0x14977, 0x04970, 0x0a4b0, 0x0b4b5, 0x06a50,
    0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970, 0x06566, 0x0d4a0,
    0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950,
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2,
    0x0a950, 0x0b557, 0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5d0, 0x14573,
    0x052b0, 0x0a9a8, 0x0e950, 0x06aa0, 0x0aea6, 0x0ab50, 0x04b60, 0x0aae4,
    0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0, 0x096d0, 0x04dd5,
    0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b5a0, 0x195a6,
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46,
    0x0ab60, 0x09570, 0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58,
    0x05ac0, 0x0ab60, 0x096d5, 0x092e0, 0x0c960, 0x0d954, 0x0d4a0, 0x0da50,
    0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5, 0x0a950, 0x0b4a0,
    0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260,
    0x0ea65, 0x0d530, 0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0,
    0x1d0b6, 0x0d250, 0x0d520, 0x0dd45, 0x0b5a0, 0x056d0, 0x055b2, 0x049b0,
    0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0, 0x14b63, 0x09370,
    0x049f8, 0x04970, 0x064b0, 0x168a6, 0x0ea50, 0x06aa0, 0x1a6c4, 0x0aae0,
    0x092e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0,
    0x0a6d0, 0x055d4, 0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50,
    0x055a0, 0x0aba4, 0x0a5b0, 0x052b0, 0x0b273, 0x06930, 0x07337, 0x06aa0,
    0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160, 0x0e968, 0x0d520,
    0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a2d0, 0x0d150, 0x0f252,
};
const int kTableYears = 200;  // 1900-2099

int LeapMonth(int year) { return kLunarInfo[year - 1900] & 0xf; }

int LeapDays(int year) {
    if (LeapMonth(year) == 0) return 0;
    return (kLunarInfo[year - 1900] & 0x10000) ? 30 : 29;
}

int MonthDays(int year, int month) {
    return (kLunarInfo[year - 1900] & (0x10000 >> month)) ? 30 : 29;
}

int LunarYearDays(int year) {
    int sum = 348;
    for (uint32_t i = 0x8000; i > 0x8; i >>= 1) {
        sum += (kLunarInfo[year - 1900] & i) ? 1 : 0;
    }
    return sum + LeapDays(year);
}

// Howard Hinnant days_from_civil:公历日期 -> 自 1970-01-01 的天数
long DaysFromCivil(long y, long m, long d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

const char* kMonthCn[] = {"正月", "二月", "三月", "四月", "五月", "六月",
                          "七月", "八月", "九月", "十月", "冬月", "腊月"};

void DayCn(int d, char* out, size_t cap) {
    static const char* digit[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
    if (d == 10) {
        snprintf(out, cap, "初十");
    } else if (d == 20) {
        snprintf(out, cap, "二十");
    } else if (d == 30) {
        snprintf(out, cap, "三十");
    } else if (d < 10) {
        snprintf(out, cap, "初%s", digit[d - 1]);
    } else if (d < 20) {
        snprintf(out, cap, "十%s", digit[d - 11]);
    } else {
        snprintf(out, cap, "廿%s", digit[d - 21]);
    }
}

}  // namespace

bool SolarToLunarCn(int year, int month, int day, char* out, size_t cap) {
    if (year < 1900 || year >= 1900 + kTableYears) return false;

    long offset = DaysFromCivil(year, month, day) - DaysFromCivil(1900, 1, 31);
    if (offset < 0) return false;

    int lunar_year = 1900, temp = 0;
    for (; lunar_year < 1900 + kTableYears && offset > 0; lunar_year++) {
        temp = LunarYearDays(lunar_year);
        offset -= temp;
    }
    if (offset < 0) {
        offset += temp;
        lunar_year--;
    }

    int leap = LeapMonth(lunar_year);
    bool is_leap = false;
    int lunar_month = 1;
    for (; lunar_month < 13 && offset > 0; lunar_month++) {
        if (leap > 0 && lunar_month == leap + 1 && !is_leap) {
            --lunar_month;
            is_leap = true;
            temp = LeapDays(lunar_year);
        } else {
            temp = MonthDays(lunar_year, lunar_month);
        }
        if (is_leap && lunar_month == leap + 1) is_leap = false;
        offset -= temp;
    }
    if (offset == 0 && leap > 0 && lunar_month == leap + 1) {
        if (is_leap) {
            is_leap = false;
        } else {
            is_leap = true;
            --lunar_month;
        }
    }
    if (offset < 0) {
        offset += temp;
        --lunar_month;
    }
    int lunar_day = (int)offset + 1;

    char day_cn[16];
    DayCn(lunar_day, day_cn, sizeof(day_cn));
    if (is_leap) {
        snprintf(out, cap, "闰%s%s", kMonthCn[lunar_month - 1], day_cn);
    } else {
        snprintf(out, cap, "%s%s", kMonthCn[lunar_month - 1], day_cn);
    }
    return true;
}
