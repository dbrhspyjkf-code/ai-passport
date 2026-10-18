#pragma once

// mail_app —— 未读邮件查看器。
// 数据:hermes /mail/status 的 messages 列表(MailStatus 缓存)。
// UI:每屏一封(发件人/主题/内容预览),上下键切换,OK 刷新。
// 长按 OK 退出(board 层)。

#include <lvgl.h>

#include "mail_status.h"

class MailApp {
public:
    static MailApp& Get();

    void Create(lv_obj_t* parent);
    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Destroy();
    void HandleKey(int key);   // 0=UP 1=DOWN 2=OK_CLICK

private:
    MailApp() = default;

    void Render();
    void Reload();   // 从 MailStatus 拷贝邮件列表

    lv_obj_t* root_ = nullptr;
    lv_obj_t* from_label_ = nullptr;
    lv_obj_t* subject_label_ = nullptr;
    lv_obj_t* body_label_ = nullptr;
    lv_obj_t* index_label_ = nullptr;   // "1/3"

    struct Msg {
        char from[72];
        char subject[84];
        char preview[304];
    };
    Msg msgs_[10];
    int msg_count_ = 0;
    int cur_ = 0;
    bool active_ = false;
};
