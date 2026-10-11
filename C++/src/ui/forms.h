// forms.h —— 自绘对话框/私聊窗口的小型工具箱（无任何标准控件）
#pragma once

#include "ui_common.h"

namespace ui {

class App;

// 对话框里的一个自绘条目
struct FItem {
    enum Kind { Label, Button, Input, TableItem, Line, Blank };
    Kind kind = Label;
    RECT rc{};
    int  id = 0;
    std::wstring text;
    int  pt = 10;
    bool bold = false;
    int  align = 0;              // 0 左 1 中 2 右
    int  valign = 1;
    bool ellipsis = true;
    bool wrap = false;
    bool hidden = false;
    bool disabled = false;
    bool primary = false;        // 绿色实心
    bool outline = false;        // 1px 实线边
    bool flat = true;            // 无边框按钮
    bool rightAlignBtn = false;  // 按钮内部文字对齐（默认居中）
    Gdiplus::Color fg = C_TEXT;
    Gdiplus::Color bg = C_PANEL;
    int  wrapW = 0;
    TextInput* input = nullptr;
    Table* table = nullptr;
};

class Form {
public:
    virtual ~Form();

    bool createForm(HINSTANCE inst, HWND owner, App* app, const std::wstring& title, int w, int h,
                    bool resizable = false);
    int  runModal();
    HWND hwnd() const { return hwnd_; }
    App* app() const { return app_; }
    void close(int result);
    void invalidate();
    void showWindow();
    TextInput* focusInput() const { return focus_; }

    UINT_PTR post(std::function<void()> fn, int ms);
    void cancelTimer(UINT_PTR id);

protected:
    virtual void buildLayout() = 0;                 // 重建 items_（客户区坐标）
    virtual void onClick(int id, int val) { (void)id; (void)val; }
    virtual void onEnterKey() {}
    virtual void onDblClick(int id, int val) { (void)id; (void)val; }
    virtual void onPaintExtra(Gdiplus::Graphics& g, int w, int h) { (void)g; (void)w; (void)h; }
    virtual void onCreated() {}
    virtual void onDestroyed() {}
    virtual bool onCloseRequest() { return true; }   // false = 拦截 WM_CLOSE
    virtual void onTimerFired(UINT_PTR id) { (void)id; }
    virtual void onCharInput(wchar_t c) { (void)c; }
    virtual bool onWheel(int x, int y, int notches) { (void)x; (void)y; (void)notches; return false; }
    virtual void onActivated() {}

    // 便捷构造
    FItem& addLabel(const std::wstring& text, int x, int y, int w, int h, int pt, bool bold,
                    const Gdiplus::Color& fg, int align = 0, bool wrap = false);
    FItem& addButton(int id, const std::wstring& text, int x, int y, int w, int h, int pt,
                     bool bold, bool primary, bool outline = true);
    FItem& addInput(TextInput* in, int x, int y, int w, int h, int pt);
    FItem& addTable(Table* t, int x, int y, int w, int h);
    FItem& addLine(int x, int y, int w);

    void relayout();
    void fitToClient(int w, int h);
    void centerOnOwner(int w, int h, int divY = 3);
    void centerRightBottom(int w, int h);
    void centerLikeChat(int w, int h);
    void setFocusInput(TextInput* t);
    void paintItems(Gdiplus::Graphics& g, int w, int h);
    void applyDefaultFont();
    int  textH(int pt, bool bold = false);
    int  textW(const std::wstring& s, int pt, bool bold = false);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    App* app_ = nullptr;
    HINSTANCE inst_ = nullptr;
    std::vector<FItem> items_;
    TextInput* focus_ = nullptr;
    int  clientW_ = 0, clientH_ = 0;
    int  result_ = 0;
    bool modal_ = true;
    bool caretOn_ = true;
    bool dragging_ = false;
    int  pressedId_ = 0;
    int  hoverId_ = 0;
    int  selRow_ = -1;
    std::map<UINT_PTR, std::function<void()>> timers_;
    UINT_PTR nextTimer_ = 500;

private:
    static LRESULT CALLBACK sWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT wndProc(HWND, UINT, WPARAM, LPARAM);
    FItem* itemAt(int x, int y);
    FItem* itemById(int id);
    void paintInput(Gdiplus::Graphics& g, FItem& it);
};

// ------------------------------------------------------------ 消息框 --
// kind: 0 信息 / 1 警告 / 2 错误
void msgBox(App* app, const std::wstring& title, const std::wstring& text, int kind = 0,
            HWND owner = nullptr);
bool askYesNo(App* app, const std::wstring& title, const std::wstring& text, HWND owner = nullptr);

// ------------------------------------------------------------ 具体对话框 --
void openCreateGroupDialog(App* app);
void openJoinGroupDialog(App* app);
void openSettingsDialog(App* app);
void showRenameVoteDialog(App* app, const nw::Json& info);
void openChangeVoteDialog(App* app, const Card& card);
void showBlankDeviceDialog(App* app);
void showUpdateDialog(App* app, const nw::Json& res);
void netCreateDialog(App* app);                                  // 建立内网（输入名称）
void netRenameDialog(App* app, const std::wstring& curName);     // 重命名内网
void showGroupMenu(App* app, const std::string& gid, int screenX, int screenY);
void hideGroupMenu(App* app);

// ------------------------------------------------------------ 设置对话框 --
// 独立声明，便于主窗口把改名投票的进度/结果转发进来（等价 app._settings_dlg）。
class SettingsForm : public Form {
public:
    explicit SettingsForm(App* app) : app_(app) {}
    void onVoteProgress(const std::string& pid, int approve, int reject);
    void onVoteDone(const std::string& pid, bool ok, const std::wstring& reason,
                    const std::wstring& newName);

protected:
    void buildLayout() override;
    void onClick(int id, int val) override;
    void onEnterKey() override;
    void onCreated() override;
    void onDestroyed() override;

private:
    void saveName();
    void applyUi(const std::wstring& newName);
    App* app_ = nullptr;
    bool voting_ = false;
    std::wstring votingNew_;
    TextInput name_;
    std::wstring savedLbl_;
    int  saveBtnId_ = 0;
    int  tipH_ = 0;
};

// ------------------------------------------------------------ 私聊窗口 --
class ChatWindow : public Form {
public:
    ChatWindow(App* app, const std::string& uid, const std::wstring& name);
    ~ChatWindow() override;

    void appendMsg(const nw::Json& m);
    void appendSys(const std::wstring& text);
    void refreshStatus();
    void markRead();
    void markUnread() { unread_++; refreshStatus(); }
    bool focused() const;
    int  unread() const { return unread_; }
    const std::string& uid() const { return uid_; }
    std::wstring titleText() const;

protected:
    void buildLayout() override;
    void onPaintExtra(Gdiplus::Graphics& g, int w, int h) override;
    void onClick(int id, int val) override;
    void onEnterKey() override;
    void onCreated() override;
    void onDestroyed() override;
    void onActivated() override;
    bool onWheel(int x, int y, int notches) override;

private:
    void send();
    struct DmMsg {
        bool sys = false;
        std::wstring from, text;
        double ts = 0.0;
        bool self = false;
        bool relayed = false;
    };
    std::vector<DmMsg> msgs_;
    TextInput entry_;
    std::string uid_;
    std::wstring name_;
    int  unread_ = 0;
    ScrollArea sc_;
    RECT rcHead_{}, rcBody_{}, rcInput_{}, rcEntry_{}, rcSend_{};
    bool online_ = false;
};

ChatWindow* openChatWindow(App* app, const std::string& uid, const std::wstring& name);

} // namespace ui
