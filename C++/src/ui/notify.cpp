// notify.cpp —— Shell_NotifyIconW 气泡通知
#include "notify.h"

namespace ui {

void Notifier::attach(HWND hwnd) {
    hwnd_ = hwnd;
    if (!hwnd_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = NOTIFY_UID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_NW_TRAY;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    std::wstring tip = U8(nw::APP_NAME);
    if (tip.size() > 127) tip = tip.substr(0, 127);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) ? true : false;
}

void Notifier::detach() {
    if (!added_ || !hwnd_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd_;
    nid.uID = NOTIFY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
    modified_ = false;
}

bool Notifier::notify(const std::wstring& title, const std::wstring& msg) {
    if (!hwnd_) return false;
    if (!added_) attach(hwnd_);
    if (!added_) return false;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd_;
    nid.uID = NOTIFY_UID;
    nid.uFlags = NIF_TIP | NIF_INFO;
    std::wstring tip = U8(nw::APP_NAME);
    if (tip.size() > 127) tip = tip.substr(0, 127);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    std::wstring t = title.size() > 63 ? title.substr(0, 63) : title;
    std::wstring m = msg.size() > 254 ? msg.substr(0, 254) : msg;
    wcsncpy_s(nid.szInfoTitle, t.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, m.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_INFO;
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) return false;
    modified_ = true;
    return true;
}

} // namespace ui
