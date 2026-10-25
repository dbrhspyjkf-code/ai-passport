#pragma once

// tools_app —— 工具文件夹选择屏(新闻/OrangePi)。
// 主页第 7 格入口;内部条目经 SwitchApp 进入(9=新闻 11=OrangePi),
// 切换即销毁本文件夹(与 GamesApp 同生命周期模式)。

#include "lvgl.h"

class ToolsApp {
public:
    static ToolsApp& Get();

    // 创建 UI(持 LVGL 锁调用)。
    void Create(lv_obj_t* parent);

    bool Active() const { return active_; }

    void Enter();
    void Leave();
    void Destroy();

    // 按键路由(board 层持锁调用):0=上一项 1=下一项 2=进入选中。
    void HandleKey(int key);

private:
    ToolsApp() = default;

    static const int kToolCount = 2;   // 新闻 / OrangePi
    bool active_ = false;
    int sel_ = 0;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* rows_[kToolCount] = {};
    void Highlight();
};
