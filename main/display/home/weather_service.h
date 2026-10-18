#pragma once

// weather_service —— Open-Meteo 实时天气拉取(无 key 免费 API)。
// 独立于 LVGL:后台 task 定时拉取,结果以原子快照暴露给 UI。
//
// 数据源: https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..
//   &current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m
//   &hourly=pm2_5&forecast_days=1&timezone=auto
// PM2.5 在 hourly 里,取当前小时的值。

#include <atomic>
#include <cstdint>
#include <string>

struct WeatherSnapshot {
    bool valid = false;          // 拉到过至少一次有效数据
    int temperature_c = 0;       // 摄氏度
    int humidity_pct = 0;        // 相对湿度 %
    char condition_cn[24];       // 天气中文(服务端直给,如"晴"/"多云")
    char city[24];               // 城市名(服务端配置,如"深圳")
    uint32_t updated_at_ms = 0;  // 最近成功时间(esp_timer_get_time()/1000)
};

// 风向角转中文方位(北/东北/东/东南/南/西南/西/西北)。
const char* WeatherWindDirCn(int deg);

// 蒲福风级近似换算(km/h -> 0-12 级)。
int WeatherWindScale(int kmh);

// WMO weather_code -> 中文短语(晴/多云/阴/小雨/雪 等)。
const char* WeatherCodeCn(int code);

// 天气服务单例:Start 后台拉取;Snapshot 返回最近一次有效数据。
class WeatherService {
public:
    static WeatherService& Get();

    // 配置拉取地址与间隔(由统一轮询 task 调用 Poll)。
    void Start(const std::string& url, int interval_minutes);

    // 单次拉取(阻塞,由轮询 task 调用)。
    bool Poll();

    WeatherSnapshot Snapshot() const { return snapshot_; }

private:
    WeatherSnapshot snapshot_{};
    std::string url_;
    int interval_minutes_ = 10;
};
