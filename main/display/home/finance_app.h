#pragma once

// finance_app —— 金融 APP:三个 Tab(指数 / 自选股 / 市场报价)。
// 数据源:hermes 局域网服务(192.168.1.200:8766):
//   /api/stocks/indices   指数(name/price/pct, group cn|global)
//   /api/stocks/portfolio 自选股(name/price/chg)
//   /api/market/overview  市场(name/value/chg)
//
// 交互:OK 长按进入/返回(idle 时);UP/DOWN 切 Tab;OK 短按刷新当前 Tab。
// 列表使用 theme 全量中文字体(动态股票名,不可用子集字体)。

#include <atomic>
#include <string>
#include <vector>

#include "lvgl.h"

class FinanceApp {
public:
    enum Key { KEY_UP, KEY_DOWN, KEY_OK, KEY_OK_LONG, KEY_UP_LONG, KEY_DOWN_LONG };

    static FinanceApp& Get();

    // 创建 LVGL 对象(持 LVGL 锁调用)。
    void Create(lv_obj_t* parent, const lv_font_t* list_font);

    bool Active() const { return active_; }

    // 进入/离开 APP。Enter/Leave 内部自行持锁。
    void Enter();
    void Leave();
    void Destroy();   // 退出并销毁 LVGL 对象

    // 按键路由(board 层调用;非 active 时忽略)。
    void HandleKey(Key key);

    // 拉取指定 tab(0指数/1自选/2市场)并更新数据与 UI。
    // 由 hermes 轮询 task 调用(调用方无需持锁,内部自行加锁)。
    void RefreshTab(int tab);

private:
    FinanceApp() = default;

    struct Item {
        std::string name;
        std::string value;   // 价格/点位(显示就绪字符串)
        float pct = 0.0f;
    };

    void SelectTab(int idx);
    void RebuildList();
    static bool FetchJsonArray(const char* url, std::vector<Item>* out, bool portfolio_style);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* tab_labels_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* list_box_ = nullptr;
    const lv_font_t* list_font_ = nullptr;
    int current_tab_ = 0;
    bool active_ = false;

    std::vector<Item> data_[3];   // 0=指数 1=自选股 2=市场
    bool tab_failed_[3] = {false, false, false};
};
