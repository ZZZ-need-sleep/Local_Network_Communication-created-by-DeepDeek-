// app.h —— 主窗口（纯 Win32 + GDI+ 自绘）：三栏布局、五个导航页、事件泵。
#pragma once

#include "notify.h"
#include "ui_common.h"

namespace ui {

class ChatWindow;

// 聊天缓存里的一条消息（等价 Python chat_cache[gid] 的元素）
struct ChatMsg {
    bool sys = false;
    std::wstring from;
    std::string from_uid;
    std::wstring text;
    double ts = 0.0;
    bool self = false;
};

// 群文件模型的一行
struct FileRow {
    std::string fid;
    std::wstring name;
    long long size = 0;
    std::wstring sender;
    std::wstring status;
    std::wstring path;
    double progress = 0.0;
};

class App {
public:
    App(nw::PeerCore* core, nw::EventQueue* events, const std::string& dataDir, HINSTANCE inst);
    ~App();

    bool createWindow();
    void run();

    HWND hwnd() const { return hwnd_; }
    HINSTANCE inst() const { return inst_; }
    nw::PeerCore* core() { return core_; }
    nw::EventQueue* events() { return events_; }
    const std::string& dataDir() const { return dataDir_; }

    // ---------------- 供对话框/私聊窗口使用 ----------------
    void switchNav(const std::string& key);
    void setChip(const std::string& key);
    void selectGid(const std::string& gid);
    void refreshGroupsList();
    void refreshPeers();
    void renderChat(bool append);
    void renderFiles();
    void renderMembers();
    void renderNetPage();
    void renderVotePage();
    void renderMorePage();
    void updateGroupHeader();
    void updateVoteBadge();
    void appendLog(const std::wstring& line);
    void clearLog();
    void openCreateGroup();
    void openJoinGroup();
    void openSettings();
    void openDm(const std::string& uid);
    void openDataDir();
    void openDownloadsDir();
    void onManualUpdate();
    void startUpdate(const std::wstring& page);
    void applyMyNameUi();
    void rebuildFileModel();
    void castVote(const std::string& req_id, int val);
    void leaveGroup(const std::string& gid);
    void notify(const std::wstring& title, const std::wstring& msg);
    void sendChat();
    void sendFiles();
    void retryDownload();
    void openSelectedFile();
    void openFileByPath(const std::wstring& path, const std::wstring& name);
    void netScan();
    void netCreate();
    void netRename();
    void netLeave();
    void netApplySelected();
    void onNetCancelApply();
    void openChangeVoteFromHistory();
    void doBlankDevicePrompt();
    void onClose();

    void invalidate();
    void invalidateAll();
    UINT_PTR post(std::function<void()> fn, int ms);
    void cancelTimer(UINT_PTR id);

    std::wstring myName() const;
    std::wstring gnameOf(const std::string& gid);
    bool inNet();
    bool updateShown() const { return updateShown_; }
    void setUpdateShown(bool v) { updateShown_ = v; }
    void setManuallyCheck(bool v) { manuallyCheck_ = v; }

    // 设置对话框（改名投票进度/结果的唯一接收者，等价 app._settings_dlg）
    void setSettingsDlg(void* p) { settingsDlg_ = p; }
    void* settingsDlg() const { return settingsDlg_; }
    void onSettingsVoteProgress(const std::string& pid, int approve, int reject);
    void onSettingsVoteDone(const std::string& pid, bool ok, const std::wstring& reason,
                            const std::wstring& newName);
    void onSettingsClosed(void* p);

    // 私聊窗口登记
    void registerDm(const std::string& uid, ChatWindow* w);
    void unregisterDm(const std::string& uid);
    ChatWindow* findDm(const std::string& uid);
    void reapForm(class Form* f);          // 非模态对话框销毁后交给主窗口回收
    std::vector<class Form*> deadForms_;

    // 投票悬浮面板
    void showVotePopup();
    void hideVotePopup();
    void refreshVotePopup();
    void scheduleVotePopupShow();
    void scheduleVotePopupHide();
    void cancelVoteHideJob();

private:
    // 窗口
    static LRESULT CALLBACK sWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT wndProc(HWND, UINT, WPARAM, LPARAM);
    void layout();
    void addBaseHits();
    void paint(HDC hdc, int w, int h);
    void paintAll(Gdiplus::Graphics& g, int w, int h);
    void ensureHits();
    void pollEvents();

    // 绘制各区域
    void paintRail(Gdiplus::Graphics& g);
    void paintList(Gdiplus::Graphics& g);
    void paintRightChat(Gdiplus::Graphics& g);
    void paintViewBox(Gdiplus::Graphics& g);
    void paintChatView(Gdiplus::Graphics& g, const RECT& box);
    void paintFilesView(Gdiplus::Graphics& g, const RECT& box);
    void paintMembersView(Gdiplus::Graphics& g, const RECT& box);
    void paintEmptyView(Gdiplus::Graphics& g, const RECT& box);
    void paintNetgateView(Gdiplus::Graphics& g, const RECT& box);
    void paintNetPage(Gdiplus::Graphics& g);
    void paintVotePage(Gdiplus::Graphics& g);
    void paintMorePage(Gdiplus::Graphics& g);
    void paintLogPage(Gdiplus::Graphics& g);
    void paintOverlay(Gdiplus::Graphics& g, int w, int h);

    // 命中
    int  hitAction(int x, int y, Hot* out) const;
    Hot* findHot(int x, int y, int act);
    void onLeftDown(int x, int y);
    void onLeftUp(int x, int y);
    void onMouseMove(int x, int y);
    void onMouseWheel(int x, int y, int delta);
    void onKeyDown(WPARAM k);
    void onChar(wchar_t c);
    void onTimerMsg(UINT_PTR id);

    // 内部
    void onGroupCardRightClick(const std::string& gid, int sx, int sy);
    void showGroupMenuAt(const std::string& gid, int x, int y);
    void hideGroupMenu();
    void showNotifyOverlay(const std::wstring& title, const std::wstring& msg);
    void updateSearchPlaceholder();
    std::vector<std::string> filteredGroups() const;
    void scrollBottomLater();
    void pushChatMsg(const std::string& gid, const ChatMsg& m);
    void onEvChat(const nw::Json& m);
    void onEvSyschat(const nw::Json& a);
    void onEvFileNew(const nw::Json& info);
    void onEvFileStatus(const nw::Json& st);
    void onEvDm(const nw::Json& m);
    void onEvDmSent(const nw::Json& info);
    void onEvUpdate(const nw::Json& res);
    void onRenameRequest(const nw::Json& info);
    void onNetChanged();
    void onNetVoteChanged();
    void onNetResult(bool ok, const std::wstring& text);

    // ---- 状态 ----
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    nw::PeerCore* core_ = nullptr;
    nw::EventQueue* events_ = nullptr;
    std::string dataDir_;
    Notifier notifier_;

    std::string currentGid_;
    std::string chip_ = "chat";
    std::string nav_ = "chat";
    std::map<std::string, std::vector<ChatMsg>> chatCache_;
    std::map<std::string, std::map<std::string, FileRow>> fileModel_;
    std::map<std::string, int> unread_;
    std::vector<std::wstring> logLines_;
    size_t renderedN_ = 0;
    // 聊天行高缓存（切群/新增消息/宽度变化时重算）
    std::vector<int> chatRowH_;
    std::string chatRowGid_;
    size_t chatRowN_ = 0;
    int  chatRowW_ = 0;

    TextInput search_;
    TextInput chatEntry_;
    TextInput* activeInput_ = nullptr;
    bool searchPh_ = true;
    bool chatEntryFocused_ = false;

    // 滚动区域
    ScrollArea scGroups_, scChat_, scMembers_, scVote_, scMore_, scNet_, scLog_;
    std::vector<ScrollArea*> scrolls_;
    std::vector<Table*> tables_;          // 本帧参与滚轮的表（页面绘制时登记）
    struct Track { ScrollArea* area; RECT rc; };
    std::vector<Track> tracks_;
    int  dragTrack_ = -1;
    int  dragStartY_ = 0;
    int  dragStartOff_ = 0;

    Table tFiles_, tNetMembers_, tNearby_, tHist_;

    // 布局矩形
    RECT rcRail_{}, rcLogo_{}, rcNav_[5]{}, rcVoteDot_{}, rcMyAvatar_{}, rcMyName_{}, rcSetBtn_{};
    RECT rcList_{}, rcListHead_{}, rcCreate_{}, rcJoin_{}, rcSearch_{}, rcListBody_{};
    RECT rcRight_{}, rcHeader_{}, rcHdName_{}, rcHdInfo_{}, rcChips_[3]{}, rcLeave_{}, rcView_{},
        rcStatus_{};

    // 投票面板/群菜单/浮层
    HWND popup_ = nullptr;
    std::vector<Hot> popupHits_;
    UINT_PTR popupShowTimer_ = 0, popupHideTimer_ = 0;
    HWND menu_ = nullptr;
    std::string menuGid_;
    std::vector<Hot> menuHits_;

    bool overlayOn_ = false;
    std::wstring overlayTitle_, overlayMsg_;
    UINT_PTR overlayTimer_ = 0;

    // 定时器池
    std::map<UINT_PTR, std::function<void()>> timers_;
    UINT_PTR nextTimer_ = 100;

    // 交互状态
    std::vector<Hot> hits_;
    bool hitsDirty_ = true;
    bool mouseOnVoteBtn_ = false;
    std::string selectedFileId_;
    std::string selectedHistId_;
    int  pressedAct_ = A_NONE;
    std::string pressedArg_;
    int  pressedVal_ = 0;
    int  hoverAct_ = A_NONE;
    std::string hoverArg_;
    int  hoverVal_ = 0;
    bool caretOn_ = true;

    // 业务状态
    bool voteDotOn_ = false;
    bool updateShown_ = false, manuallyCheck_ = false, updating_ = false;
    void* settingsDlg_ = nullptr;
    std::map<std::string, ChatWindow*> dmWindows_;
    bool blankPromptDone_ = false;
    bool closing_ = false;
    bool netScanning_ = false;
    std::wstring netStatusText_;
    std::wstring statusLine_;
    std::wstring netApplyLbl_;
    int  chatCacheMax_ = 2000;
};

} // namespace ui
