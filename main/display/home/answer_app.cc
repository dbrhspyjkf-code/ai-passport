// answer_app.cc —— 答案之书实现。
#include "answer_app.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "answer";

// 夜空风配色(M5-DIAL 同款基调)
#define C_NIGHT_BG 0x222130     // 深蓝紫底
#define C_STAR 0x46527e         // 星点
#define C_TITLE 0x8ae0f7        // 标题青
#define C_TEXT 0x9fa3c8         // 提示文字
#define C_ANSWER 0x8b87f0       // 翻页中答案(暗)
#define C_REVEAL 0x8ae0f7       // 定格答案(亮青)

// 答案集(与 M5-DIAL answers_data.h 同源,~120 条,全部 ≤8 汉字)
static const char* kAnswers[] = {
    "是的", "是", "当然是", "毫无疑问", "无疑", "当然可以",
    "非它不可", "就是这样", "没错", "一切都会好的",
    "不", "不是", "绝不", "绝对不行", "不行", "想都别想",
    "不可能", "没必要", "多此一举", "现在不行",
    "也许", "可能", "未必", "不好说", "大概吧", "不一定",
    "一言难尽", "无可奉告", "天机不可泄露", "再问一次",
    "换个问题", "答案在你心里",
    "等等看", "耐心等待", "再等等", "时机未到", "为时过早",
    "不要急", "急也没用", "明天再说", "睡一觉再说", "静观其变",
    "等待信号", "好事多磨", "时来运转", "否极泰来",
    "顺其自然", "随它去", "放手吧", "放下它", "忘掉它",
    "别管它", "顺其发展", "水到渠成", "瓜熟蒂落", "接受吧",
    "相信你的直觉", "跟着感觉走", "相信自己", "倾听内心",
    "你已经知道了", "你心里有数", "问问朋友", "听听建议",
    "自己决定", "取决于你", "由你决定", "你说了算",
    "大胆去做", "放手去做", "去做吧", "马上行动", "立刻行动",
    "现在就做", "不要犹豫", "别想了 去做", "试一试", "试试看",
    "再试一次", "全力以赴", "坚持下去", "别放弃", "再撑一下",
    "改变方向", "换个思路", "换个角度", "退一步", "停下来",
    "从头再来", "重新开始", "另起炉灶", "打破现状", "保持现状",
    "保持专注", "集中精力", "保持沉默", "什么都不说",
    "说出真相", "坦白吧", "保守秘密", "别声张", "再想想",
    "冷静一下", "放轻松", "深呼吸", "别想太多", "别太认真",
    "小心为妙", "三思而后行", "稳一手", "认真点",
    "值得冒险", "冒个险", "机会来了", "机会稍纵即逝",
    "把握机会", "错过了", "运气不错", "好运相伴", "别指望运气",
    "没有回头路", "不要回头", "更好的在后面", "未知的更好",
    "你比想象中强", "没那么糟", "意义非凡", "结果会好",
};
static const int kAnswerCount = (int)(sizeof(kAnswers) / sizeof(kAnswers[0]));

AnswerApp& AnswerApp::Get() {
    static AnswerApp instance;
    return instance;
}

void AnswerApp::Create(lv_obj_t* parent) {
    if (root_ != nullptr) return;

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(root_, lv_color_hex(C_NIGHT_BG), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // 星点(固定伪随机布局)
    static const int kStars[8][2] = {
        {30, 40}, {190, 50}, {210, 130}, {20, 150},
        {80, 200}, {160, 30}, {50, 100}, {200, 220},
    };
    for (int i = 0; i < 8; i++) {
        lv_obj_t* star = lv_obj_create(root_);
        lv_obj_set_size(star, 2, 2);
        lv_obj_set_style_bg_color(star, lv_color_hex(C_STAR), 0);
        lv_obj_set_style_bg_opa(star, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(star, 0, 0);
        lv_obj_set_style_radius(star, 0, 0);
        lv_obj_set_scrollbar_mode(star, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(star, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(star, kStars[i][0], kStars[i][1]);
    }

    title_ = lv_label_create(root_);
    lv_obj_set_style_text_color(title_, lv_color_hex(C_TITLE), 0);
    lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 56);

    answer_label_ = lv_label_create(root_);
    lv_obj_set_width(answer_label_, LV_HOR_RES - 40);
    lv_obj_set_style_text_align(answer_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(answer_label_, LV_ALIGN_CENTER, 0, -6);

    hint_label_ = lv_label_create(root_);
    lv_obj_set_style_text_color(hint_label_, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint_label_, LV_HOR_RES - 40);
    lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_MID, 0, -34);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void AnswerApp::ShowPrompt() {
    lv_label_set_text(title_, "答案之书");
    lv_label_set_text(answer_label_, "");
    lv_label_set_text(hint_label_, "在心中默想你的问题\n按 OK 翻开答案 · 长按退出");
    lv_obj_set_style_text_color(answer_label_, lv_color_hex(C_ANSWER), 0);
}

void AnswerApp::StartFlip() {
    if (flipping_) return;
    flipping_ = true;
    flip_step_ = 0;
    lv_label_set_text(title_, "");
    lv_label_set_text(hint_label_, "");
    lv_obj_set_style_text_color(answer_label_, lv_color_hex(C_ANSWER), 0);
    if (flip_timer_ == nullptr) {
        flip_timer_ = lv_timer_create(FlipTickCb, 60, this);
    } else {
        lv_timer_set_period(flip_timer_, 60);
        lv_timer_reset(flip_timer_);
        lv_timer_resume(flip_timer_);
    }
}

void AnswerApp::FlipTickCb(lv_timer_t* t) {
    static_cast<AnswerApp*>(lv_timer_get_user_data(t))->FlipTick();
}

void AnswerApp::FlipTick() {
    // 14 步轮转,每步间隔递增(45→150ms)模拟减速
    flip_step_++;
    if (flip_step_ >= 14) {
        FinishFlip();
        return;
    }
    int idx = esp_random() % kAnswerCount;
    lv_label_set_text(answer_label_, kAnswers[idx]);
    int period = 45 + flip_step_ * 8;
    lv_timer_set_period(flip_timer_, period);
}

void AnswerApp::FinishFlip() {
    lv_timer_pause(flip_timer_);
    flipping_ = false;
    // 定格答案,避开上一条
    int idx;
    do {
        idx = esp_random() % kAnswerCount;
    } while (idx == last_answer_ && kAnswerCount > 1);
    last_answer_ = idx;
    lv_label_set_text(answer_label_, kAnswers[idx]);
    lv_obj_set_style_text_color(answer_label_, lv_color_hex(C_REVEAL), 0);
    lv_label_set_text(hint_label_, "按 OK 再问一次");
}

void AnswerApp::HandleKey(int key) {
    if (!active_ || root_ == nullptr) return;
    if (key == 2) {
        StartFlip();
    }
    // 长按退出由 board 层处理
}

void AnswerApp::Enter() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && !active_) {
            lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root_);
            active_ = true;
            ShowPrompt();
        }
        lvgl_port_unlock();
    }
}

void AnswerApp::Destroy() {
    Leave();
    if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        if (root_ != nullptr) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
        lvgl_port_unlock();
    }
}

void AnswerApp::Leave() {
    if (lvgl_port_lock(pdMS_TO_TICKS(500))) {
        if (root_ != nullptr && active_) {
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
            active_ = false;
            if (flip_timer_ != nullptr) lv_timer_pause(flip_timer_);
            flipping_ = false;
        }
        lvgl_port_unlock();
    }
}
