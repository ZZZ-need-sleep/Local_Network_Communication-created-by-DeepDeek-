// notify.h —— 系统通知（Shell_NotifyIconW 气泡；失败时退化为窗口内浮层）
#pragma once

#include "ui_common.h"

namespace ui {

class Notifier {
public:
    void attach(HWND hwnd);
    void detach();
    // 返回 true = 托盘气泡已发出；false = 调用方应退化为窗口内浮层
    bool notify(const std::wstring& title, const std::wstring& msg);
    bool added() const { return added_; }
private:
    HWND hwnd_ = nullptr;
    bool added_ = false;
    bool modified_ = false;
};

// 托盘图标 id 与回调消息（与 Python SystemNotifier 完全一致）
const UINT NOTIFY_UID = 777;
const UINT WM_NW_TRAY = WM_APP + 50;

} // namespace ui
