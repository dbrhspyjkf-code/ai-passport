#pragma once

// ipo_service —— 新股申购查询(hermes /stocks/ipo)。
// GET {base}/stocks/ipo → {"ok":true,"items":[{"name":"..","date":"11-04","today":bool}]}
// 主页 GLM 下方一行显示;随 GLM 节奏(poller)拉取。

#include <atomic>
#include <string>

class IpoService {
public:
    static IpoService& Get();

    void Start(const std::string& base_url);
    void Poll();

    bool Valid() const { return valid_; }
    uint32_t Version() const { return version_.load(); }

    // 格式化为一行文字(如"新股 菊乐股份 11-04");无申购返回空串
    const char* Text() const { return text_; }

private:
    IpoService() = default;

    char text_[64] = {};
    bool valid_ = false;
    std::atomic<uint32_t> version_{0};
    std::string base_url_;
};
