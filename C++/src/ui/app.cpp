// app.cpp —— 主窗口：窗口过程、三栏布局、绘制、输入、事件泵
#include "app.h"

#include "forms.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

using namespace Gdiplus;

namespace ui {

static const wchar_t* kWndClass = L"NwMainWnd";
static const int kNav = 5;
static const wchar_t* kNavIcon[kNav] = {L"\U0001F4AC", L"\U0001F4E1", L"\U0001F5F3", L"\u22EF",
                                        L"\U0001F4CB"};
static const wchar_t* kNavLabel[kNav] = {L"聊天", L"内网", L"投票", L"更多", L"日志"};
static const char* kNavKey[kNav] = {"chat", "net", "vote", "more", "log"};
static const wchar_t* kChipLabel[3] = {L"聊天", L"群文件", L"成员"};
static const char* kChipKey[3] = {"chat", "files", "members"};

// ---------------------------------------------------------------- 小工具 --
int g_textH(int pt, bool bold) {
    return (int)measureText(measureG(), L"字", fontPt(pt, bold)).Height;
}

int g_textW(const std::wstring& s, int pt, bool bold) {
    if (s.empty()) return 0;
    return (int)measureText(measureG(), s, fontPt(pt, bold)).Width;
}

static int charW(int pt) { return g_textW(L"0", pt, false); }

static void drawBtn(Graphics& g, const RECT& rc, const std::wstring& text, int pt, bool bold,
                    bool primary, bool outline, bool hover, const Gdiplus::Color& bgIn,
                    const Gdiplus::Color& fgIn) {
    drawStdButton(g, rc, text, pt, bold, primary, outline, hover, false, bgIn, fgIn);
}

// 统一的一组基础热点（与 layout() 保持一致）
void App::addBaseHits() {
    Hot h;
    for (int i = 0; i < kNav; ++i) {
        h.rc = rcNav_[i];
        h.act = A_NAV_CHAT + i;
        hits_.push_back(h);
    }
    h.act = A_MY_AVATAR; h.rc = rcMyAvatar_; hits_.push_back(h);
    h.act = A_SETTINGS_BTN; h.rc = rcSetBtn_; hits_.push_back(h);
    h.act = A_CREATE_GROUP; h.rc = rcCreate_; hits_.push_back(h);
    h.act = A_JOIN_GROUP; h.rc = rcJoin_; hits_.push_back(h);
    h.act = A_SEARCH_FOCUS; h.rc = rcSearch_; hits_.push_back(h);
    h.act = A_CHIP_CHAT; h.rc = rcChips_[0]; hits_.push_back(h);
    h.act = A_CHIP_FILES; h.rc = rcChips_[1]; hits_.push_back(h);
    h.act = A_CHIP_MEMBERS; h.rc = rcChips_[2]; hits_.push_back(h);
    h.act = A_LEAVE_GROUP; h.rc = rcLeave_; hits_.push_back(h);
}

// ------------------------------------------------------------ 生命周期 --
App::App(nw::PeerCore* core, nw::EventQueue* events, const std::string& dataDir, HINSTANCE inst)
    : inst_(inst), core_(core), events_(events), dataDir_(dataDir) {
    scrolls_ = {&scGroups_, &scChat_, &scMembers_, &scVote_, &scMore_, &scNet_, &scLog_};
    search_.setPlaceholder(L"\U0001F50D  搜索群组");
    search_.phText = L"\U0001F50D  搜索群组";
}

App::~App() {
    if (popup_) { DestroyWindow(popup_); popup_ = nullptr; }
    if (menu_) { DestroyWindow(menu_); menu_ = nullptr; }
    std::vector<ChatWindow*> live;
    for (auto& kv : dmWindows_) live.push_back(kv.second);
    dmWindows_.clear();
    for (auto* w : live) {
        if (w && w->hwnd()) DestroyWindow(w->hwnd());
    }
    for (Form* f : deadForms_) delete f;
    deadForms_.clear();
}

bool App::createWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &App::sWndProc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    std::wstring title = U8(nw::APP_NAME);
    title += L" v";
    title += U8(nw::APP_VERSION_FALLBACK);
    title += L"  \u00b7  \u5c40\u57df\u7f51\u7fa4\u7ec4";

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r{0, 0, 1180, 740};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - ww) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    hwnd_ = CreateWindowExW(0, kWndClass, title.c_str(), style, x, y, ww, wh, nullptr, nullptr,
                            inst_, this);
    if (!hwnd_) return false;

    // Win11 圆角：DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2
    int pref = 2;
    DwmSetWindowAttribute(hwnd_, 33, &pref, sizeof(pref));

    layout();
    notifier_.attach(hwnd_);
    SetTimer(hwnd_, 1, 100, nullptr);      // 事件泵
    SetTimer(hwnd_, 2, 530, nullptr);      // 光标闪烁
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    // 窗口显示后：空设备启动提示（design_v2 第 3 节）
    post([this]() { doBlankDevicePrompt(); }, 400);
    return true;
}

void App::run() {
    core_->check_update(false);            // 每次启动自动检查
    refreshGroupsList();
    refreshPeers();
    rebuildFileModel();
    updateVoteBadge();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK App::sWndProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    App* self = nullptr;
    if (m == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        self = (App*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->hwnd_ = hwnd;
    } else {
        self = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (self) return self->wndProc(hwnd, m, w, l);
    return DefWindowProcW(hwnd, m, w, l);
}

void App::invalidate() {
    hitsDirty_ = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::invalidateAll() {
    hitsDirty_ = true;
    if (hwnd_) RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

UINT_PTR App::post(std::function<void()> fn, int ms) {
    UINT_PTR id = nextTimer_++;
    timers_[id] = std::move(fn);
    SetTimer(hwnd_, id, (UINT)ms, nullptr);
    return id;
}

void App::cancelTimer(UINT_PTR id) {
    if (!id) return;
    KillTimer(hwnd_, id);
    timers_.erase(id);
}

void App::onTimerMsg(UINT_PTR id) {
    if (id == 1) {
        pollEvents();
        return;
    }
    if (id == 2) {
        caretOn_ = !caretOn_;
        if (activeInput_) invalidate();
        return;
    }
    auto it = timers_.find(id);
    if (it == timers_.end()) return;
    std::function<void()> fn = it->second;
    timers_.erase(it);
    KillTimer(hwnd_, id);
    fn();
}

LRESULT App::wndProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr;
        GetClientRect(hwnd, &cr);
        paint(hdc, cr.right, cr.bottom);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        layout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        RECT r{0, 0, 980, 620};
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_TIMER:
        onTimerMsg((UINT_PTR)w);
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        SetFocus(hwnd);
        onLeftDown(x, y);
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        onLeftUp(x, y);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        ensureHits();
        Hot h;
        if (hitAction(x, y, &h) == A_FILE_ROW) {
            tFiles_.sel = tFiles_.rowAt(x, y);
            if (tFiles_.sel >= 0 && tFiles_.sel < (int)tFiles_.iids.size())
                selectedFileId_ = tFiles_.iids[(size_t)tFiles_.sel];
            openSelectedFile();
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        ensureHits();
        Hot h;
        if (hitAction(x, y, &h) == A_GROUP_CARD) {
            POINT p{x, y};
            ClientToScreen(hwnd, &p);
            showGroupMenuAt(h.arg, p.x, p.y);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        onMouseMove(x, y);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(hwnd, &p);
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        onMouseWheel(p.x, p.y, delta);
        return 0;
    }
    case WM_CHAR:
    case WM_IME_CHAR:
        if (w >= 32 || w == 8) onChar((wchar_t)w);
        return 0;
    case WM_KEYDOWN:
        onKeyDown(w);
        return 0;
    case WM_SETCURSOR: {
        if (LOWORD(l) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            Hot h;
            if (hitAction(p.x, p.y, &h) != A_NONE) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }
    case WM_CLOSE:
        onClose();
        return 0;
    case WM_DESTROY:
        notifier_.detach();
        PostQuitMessage(0);
        return 0;
    case WM_NW_TRAY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

void App::onClose() {
    if (closing_) return;
    closing_ = true;
    hideVotePopup();
    core_->stop();                 // 退出时保存数据
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

// ---------------------------------------------------------------- 布局 --
void App::layout() {
    if (!hwnd_) return;
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int W = cr.right, H = cr.bottom;

    rcRail_ = RECT{0, 0, 64, H};
    rcList_ = RECT{64, 0, 364, H};
    rcRight_ = RECT{364, 0, W, H};

    // ---- 左侧图标栏 ----
    int logoW = g_textW(L"群", 15, true) * 2;
    int logoH = g_textH(15, true) + 8;
    int ly = 14;
    rcLogo_ = RECT{(64 - logoW) / 2, ly, (64 - logoW) / 2 + logoW, ly + logoH};
    ly += logoH + 12;
    for (int i = 0; i < kNav; ++i) {
        int iconH = (int)measureText(measureG(), kNavIcon[i], emojiFontPt(16)).Height;
        int labH = g_textH(8, false);
        int blockH = iconH + labH;
        int iconW = (int)measureText(measureG(), kNavIcon[i], emojiFontPt(16)).Width;
        int labW = g_textW(kNavLabel[i], 8, false);
        int innerW = (std::max)(iconW, labW);
        int frameW = innerW + 4;                      // ipadx=2
        int fx = (64 - frameW) / 2;
        rcNav_[i] = RECT{fx, ly, fx + frameW, ly + blockH};
        ly += blockH + 6;                              // pady=3
    }
    {
        // 投票红点：锚点 NE，位于按钮右上角（x = 右边 -3，y = 顶 +1）
        int bx = rcNav_[2].right - 3, by = rcNav_[2].top + 1;
        rcVoteDot_ = RECT{bx - 11, by, bx, by + 11};
    }
    // 底部：设置 / 名字 / 头像（自下而上）
    {
        int gH = g_textH(13, false) + g_textH(8, false);
        int gW = (std::max)(g_textW(L"\u2699", 13, false), g_textW(L"设置", 8, false));
        int gy = H - 12 - gH;
        rcSetBtn_ = RECT{(64 - gW) / 2, gy, (64 - gW) / 2 + gW, gy + gH};
        std::wstring nm = leftChars(myName(), 4);
        int nH = g_textH(8, false);
        int nW = g_textW(nm, 8, false) + 4;
        int ny = gy - 6 - nH;
        rcMyName_ = RECT{(64 - nW) / 2, ny, (64 - nW) / 2 + nW, ny + nH};
        int ay = ny - 2 - 36;
        rcMyAvatar_ = RECT{(64 - 36) / 2, ay, (64 - 36) / 2 + 36, ay + 36};
    }

    // ---- 中间会话列表 ----
    int contentR = 363;                                  // 300 宽 - 右侧 1px 边线
    rcListHead_ = RECT{64 + 14, 16, contentR, 16 + g_textH(15, true)};
    int by = rcListHead_.bottom + 8;
    int createH = g_textH(10, true) + 8;
    int joinH = g_textH(10, false) + 6;
    int rowH = (std::max)(createH, joinH);
    int joinW = charW(10) * 9 + 6;
    rcJoin_ = RECT{contentR - 12 - joinW, by, contentR - 12, by + joinH};
    rcCreate_ = RECT{64 + 12, by, rcJoin_.left - 6, by + createH};
    int sy = by + rowH + 2;
    int sh = g_textH(10, false) + 10;                    // ipady=5
    rcSearch_ = RECT{64 + 12, sy, contentR - 12, sy + sh};
    rcListBody_ = RECT{64, rcSearch_.bottom + 8, contentR, H};

    // ---- 右侧 ----
    rcHeader_ = RECT{364, 0, W, 64};
    rcHdName_ = RECT{364 + 18, 6, 364 + 18 + 400, 6 + g_textH(14, true)};
    int nameW = g_textW(gnameOf(currentGid_), 14, true);
    rcHdInfo_ = RECT{rcHdName_.left + nameW + 8, 10, rcHdName_.left + nameW + 8 + 400,
                     10 + g_textH(9, false)};
    {
        int barR = W - 16;
        int cw[3];
        int barW = 0;
        for (int i = 0; i < 3; ++i) {
            cw[i] = g_textW(kChipLabel[i], 10, false) + 28;
            barW += cw[i] + 4;
        }
        int cx = barR - barW;
        for (int i = 0; i < 3; ++i) {
            rcChips_[i] = RECT{cx, 10, cx + cw[i], 10 + g_textH(10, false) + 8};
            cx += cw[i] + 4;
        }
        int lw = g_textW(L"退出群组", 9, false) + 8;
        rcLeave_ = RECT{rcChips_[0].left - 14 - lw, 16, rcChips_[0].left - 14,
                        16 + g_textH(9, false) + 4};
    }
    rcStatus_ = RECT{364, H - 26, W, H};
    rcView_ = RECT{364, 65, W, H - 26};

    // ---- 滚动区域尺寸 ----
    scGroups_.rc = rcListBody_;
    scGroups_.contentH = (int)filteredGroups().size() * 62 + 8;
    scGroups_.clamp();
    for (ScrollArea* a : scrolls_) {
        if (a != &scGroups_) a->clamp();
    }
    hitsDirty_ = true;
}

// ---------------------------------------------------------------- 绘制 --
void App::paint(HDC hdc, int w, int h) {
    DblBuffer db;
    if (!db.begin(hdc, w, h)) return;
    Graphics g(db.mem);
    paintAll(g, w, h);
    db.end();
    db.release();
}

// 命中区与最近一次绘制保持一致：任何状态变化都会 invalidate()；此时
// ensureHits() 会在 16x16 离屏位图上跑一遍完整绘制路径（绘制被裁掉），
// 只为重建热点与滚动条矩形。
void App::ensureHits() {
    if (!hitsDirty_ || !hwnd_) return;
    RECT cr;
    GetClientRect(hwnd_, &cr);
    HDC hdc = GetDC(hwnd_);
    DblBuffer db;
    if (db.begin(hdc, 16, 16)) {
        Graphics g(db.mem);
        paintAll(g, cr.right, cr.bottom);
        db.release();
    }
    ReleaseDC(hwnd_, hdc);
    hitsDirty_ = false;
}

void App::paintAll(Graphics& g, int w, int h) {
    hits_.clear();
    tracks_.clear();
    tables_.clear();
    addBaseHits();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    fillRect(g, Gdiplus::RectF(0, 0, (REAL)w, (REAL)h), C_BG);

    paintRail(g);
    paintList(g);
    if (nav_ == "chat") {
        paintRightChat(g);
    } else if (nav_ == "net") {
        paintNetPage(g);
    } else if (nav_ == "vote") {
        paintVotePage(g);
    } else if (nav_ == "more") {
        paintMorePage(g);
    } else {
        paintLogPage(g);
    }
    paintOverlay(g, w, h);
    hitsDirty_ = false;
}

void App::paintRail(Graphics& g) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int H = cr.bottom;
    fillRect(g, Gdiplus::RectF(0, 0, 64, (REAL)H), C_PANEL);
    drawVDivider(g, 63, 0, H, C_BORDER);
    fillRect(g, Gdiplus::RectF((REAL)rcLogo_.left, (REAL)rcLogo_.top,
                               (REAL)(rcLogo_.right - rcLogo_.left),
                               (REAL)(rcLogo_.bottom - rcLogo_.top)), C_ACCENT);
    drawText(g, L"群", fontPt(15, true), C_WHITE,
             Gdiplus::RectF((REAL)rcLogo_.left, (REAL)rcLogo_.top,
                            (REAL)(rcLogo_.right - rcLogo_.left),
                            (REAL)(rcLogo_.bottom - rcLogo_.top)), 1, 1, false, false);
    for (int i = 0; i < kNav; ++i) {
        bool sel = (nav_ == kNavKey[i]);
        bool hov = (hoverAct_ == A_NAV_CHAT + i);
        Gdiplus::RectF rf((REAL)rcNav_[i].left, (REAL)rcNav_[i].top,
                          (REAL)(rcNav_[i].right - rcNav_[i].left),
                          (REAL)(rcNav_[i].bottom - rcNav_[i].top));
        if (sel) fillRect(g, rf, C_SELECTED);
        else if (hov) fillRect(g, rf, C_HOVER);
        RECT r = rcNav_[i];
        int blockH = r.bottom - r.top;
        int iconH = (int)measureText(measureG(), kNavIcon[i], emojiFontPt(16)).Height;
        drawText(g, kNavIcon[i], emojiFontPt(16), C_TEXT,
                 Gdiplus::RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)iconH),
                 1, 1, false, false);
        drawText(g, kNavLabel[i], fontPt(8, false), C_SUB,
                 Gdiplus::RectF((REAL)r.left, (REAL)(r.top + iconH), (REAL)(r.right - r.left),
                                (REAL)(blockH - iconH)),
                 1, 1, false, false);
    }
    if (voteDotOn_) {
        Gdiplus::RectF d((REAL)rcVoteDot_.left + 1, (REAL)rcVoteDot_.top + 1, 9, 9);
        fillCircle(g, d, C_BADGE);
    }
    drawAvatar(g, (REAL)rcMyAvatar_.left, (REAL)rcMyAvatar_.top, 36, myName());
    drawText(g, leftChars(myName(), 4), fontPt(8, false), C_SUB,
             Gdiplus::RectF((REAL)rcMyName_.left, (REAL)rcMyName_.top,
                            (REAL)(rcMyName_.right - rcMyName_.left),
                            (REAL)(rcMyName_.bottom - rcMyName_.top)),
             1, 1, false, false);
    bool setHov = (hoverAct_ == A_SETTINGS_BTN);
    int gh = g_textH(13, false);
    drawText(g, L"\u2699", fontPt(13, false), C_SUB,
             Gdiplus::RectF((REAL)rcSetBtn_.left, (REAL)rcSetBtn_.top,
                            (REAL)(rcSetBtn_.right - rcSetBtn_.left), (REAL)gh),
             1, 1, false, false);
    drawText(g, L"设置", fontPt(8, false), setHov ? C_TEXT : C_SUB,
             Gdiplus::RectF((REAL)rcSetBtn_.left, (REAL)(rcSetBtn_.top + gh),
                            (REAL)(rcSetBtn_.right - rcSetBtn_.left),
                            (REAL)(rcSetBtn_.bottom - rcSetBtn_.top - gh)),
             1, 1, false, false);
}

void App::paintList(Graphics& g) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int H = cr.bottom;
    fillRect(g, Gdiplus::RectF(64, 0, 300, (REAL)H), C_PANEL);
    drawVDivider(g, 363, 0, H, C_BORDER);
    drawText(g, L"群聊", fontPt(15, true), C_TEXT,
             Gdiplus::RectF((REAL)rcListHead_.left, (REAL)rcListHead_.top,
                            (REAL)(rcListHead_.right - rcListHead_.left),
                            (REAL)(rcListHead_.bottom - rcListHead_.top)),
             0, 1, false, false);
    drawBtn(g, rcCreate_, L"＋ 创建群组", 10, true, true, false, hoverAct_ == A_CREATE_GROUP,
            C_ACCENT, C_WHITE);
    drawBtn(g, rcJoin_, L"加入群组", 10, false, false, true, hoverAct_ == A_JOIN_GROUP, C_PANEL,
            C_ACCENT);
    fillRect(g, Gdiplus::RectF((REAL)rcSearch_.left, (REAL)rcSearch_.top,
                               (REAL)(rcSearch_.right - rcSearch_.left),
                               (REAL)(rcSearch_.bottom - rcSearch_.top)), C_ENTRY_BG);
    {
        Gdiplus::Color c = search_.placeholder ? C_SUB : C_TEXT;
        int tx = rcSearch_.left + 8;
        drawText(g, search_.text, fontPt(10, false), c,
                 Gdiplus::RectF((REAL)tx, (REAL)rcSearch_.top,
                                (REAL)(rcSearch_.right - tx - 6),
                                (REAL)(rcSearch_.bottom - rcSearch_.top)),
                 0, 1, false, !search_.placeholder);
        if (search_.focused && caretOn_) {
            int cx = search_.caretX(measureG(), fontPt(10, false), tx);
            drawVDivider(g, cx, rcSearch_.top + 3, rcSearch_.bottom - 3, C_TEXT);
        }
    }
    std::vector<std::string> gids = filteredGroups();
    int y = rcListBody_.top + 8 - scGroups_.offset;
    for (size_t gi = 0; gi < gids.size(); ++gi) {
        const std::string gid = gids[gi];
        auto itg = core_->groups.find(gid);
        if (itg == core_->groups.end()) continue;
        std::wstring gname = U8(itg->second.get("gname").asStr(""));
        int cardH = 58;
        RECT card{72, y, 355, y + cardH};
        if (card.bottom > rcListBody_.top && card.top < rcListBody_.bottom) {
            bool sel = (gid == currentGid_);
            bool hov = (hoverAct_ == A_GROUP_CARD && hoverArg_ == gid);
            Gdiplus::Color bgc = sel ? C_SELECTED : (hov ? C_HOVER : C_PANEL);
            fillRect(g, Gdiplus::RectF((REAL)card.left, (REAL)card.top,
                                       (REAL)(card.right - card.left),
                                       (REAL)(card.bottom - card.top)), bgc);
            drawAvatar(g, (REAL)(card.left + 10), (REAL)(card.top + 9), 40, gname);
            int midX = card.left + 10 + 40 + 8;
            int midR = card.right - 12;
            drawText(g, gname, fontPt(11, true), C_TEXT,
                     Gdiplus::RectF((REAL)midX, (REAL)(card.top + 7), (REAL)(midR - midX),
                                    (REAL)g_textH(11, true)),
                     0, 1, false, true);
            std::wstring prev = L"点击进入群聊";
            std::wstring tstr;
            auto ci = chatCache_.find(gid);
            if (ci != chatCache_.end()) {
                for (auto rit = ci->second.rbegin(); rit != ci->second.rend(); ++rit) {
                    if (!rit->sys) {
                        std::wstring t = rit->from + L": " + rit->text;
                        prev = leftChars(t, 28);
                        if (prev.empty()) prev = L"…";
                        tstr = timeHMS(rit->ts);
                        break;
                    }
                }
            }
            int timeW = tstr.empty() ? 0 : g_textW(tstr, 9, false);
            int subY = card.top + 7 + g_textH(11, true) + 1;
            drawText(g, prev, fontPt(9, false), C_SUB,
                     Gdiplus::RectF((REAL)midX, (REAL)subY, (REAL)(midR - midX - timeW - 6),
                                    (REAL)g_textH(9, false)),
                     0, 1, false, true);
            if (!tstr.empty()) {
                drawText(g, tstr, fontPt(9, false), C_SUB,
                         Gdiplus::RectF((REAL)(card.right - 12 - timeW), (REAL)(card.top + 8),
                                        (REAL)timeW, (REAL)g_textH(9, false)),
                         2, 1, false, false);
            }
            int un = unread_.count(gid) ? unread_[gid] : 0;
            if (un > 0) {
                std::wstring bs = un < 100 ? std::to_wstring(un) : L"99+";
                int bw = g_textW(bs, 8, true) + 10;
                int bh = g_textH(8, true) + 2;
                int bx = card.right - 12 - bw;
                int byy = card.top + 8 + g_textH(9, false) + 4;
                Gdiplus::RectF br((REAL)bx, (REAL)byy, (REAL)bw, (REAL)bh);
                fillRound(g, br, 3.f, C_BADGE);
                drawText(g, bs, fontPt(8, true), C_WHITE, br, 1, 1, false, false);
            }
        }
        Hot h;
        h.rc = card;
        h.act = A_GROUP_CARD;
        h.arg = gid;
        hits_.push_back(h);
        y += cardH + 4;
    }
    if (scGroups_.contentH > scGroups_.viewH()) {
        RECT track{scGroups_.rc.right - SCROLLBAR_W, scGroups_.rc.top, scGroups_.rc.right,
                   scGroups_.rc.bottom};
        drawScrollBar(g, track, scGroups_.contentH, scGroups_.viewH(), scGroups_.offset, false);
        Track t{&scGroups_, track};
        tracks_.push_back(t);
    }
}

void App::paintRightChat(Graphics& g) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int W = cr.right;
    fillRect(g, Gdiplus::RectF(364, 0, (REAL)(W - 364), (REAL)cr.bottom), C_BG);
    fillRect(g, Gdiplus::RectF(364, 0, (REAL)(W - 364), 64), C_PANEL);
    drawHDivider(g, 364, 63, W, C_BORDER);
    drawText(g, gnameOf(currentGid_), fontPt(14, true), C_TEXT,
             Gdiplus::RectF((REAL)rcHdName_.left, (REAL)rcHdName_.top,
                            (REAL)(rcHdName_.right - rcHdName_.left),
                            (REAL)(rcHdName_.bottom - rcHdName_.top)),
             0, 1, false, false);
    if (!currentGid_.empty()) {
        auto it = core_->groups.find(currentGid_);
        if (it != core_->groups.end()) {
            std::vector<nw::Json> mem = core_->group_members(currentGid_);
            wchar_t buf[256];
            swprintf(buf, 256, L"群号 %s · 在线 %d 人", U8(currentGid_).c_str(), (int)mem.size());
            drawText(g, buf, fontPt(9, false), C_SUB,
                     Gdiplus::RectF((REAL)rcHdInfo_.left, (REAL)rcHdInfo_.top,
                                    (REAL)(rcHdInfo_.right - rcHdInfo_.left),
                                    (REAL)(rcHdInfo_.bottom - rcHdInfo_.top)),
                     0, 1, false, false);
        }
    }
    for (int i = 0; i < 3; ++i) {
        bool active = (chip_ == kChipKey[i]);
        bool hov = (hoverAct_ == A_CHIP_CHAT + i);
        Gdiplus::RectF r((REAL)rcChips_[i].left, (REAL)rcChips_[i].top,
                         (REAL)(rcChips_[i].right - rcChips_[i].left),
                         (REAL)(rcChips_[i].bottom - rcChips_[i].top));
        fillRect(g, r, active ? C_SELECTED : (hov ? C_HOVER : C_PANEL));
        drawText(g, kChipLabel[i], fontPt(10, active), active ? C_ACCENT : C_SUB, r, 1, 1, false,
                 false);
    }
    drawBtn(g, rcLeave_, L"退出群组", 9, false, false, false, hoverAct_ == A_LEAVE_GROUP, C_PANEL,
            C_SUB);
    paintViewBox(g);
    fillRect(g, Gdiplus::RectF((REAL)rcStatus_.left, (REAL)rcStatus_.top,
                               (REAL)(rcStatus_.right - rcStatus_.left),
                               (REAL)(rcStatus_.bottom - rcStatus_.top)), C_PANEL);
    drawHDivider(g, rcStatus_.left, rcStatus_.top, rcStatus_.right, C_BORDER);
    drawText(g, statusLine_, fontPt(9, false), C_SUB,
             Gdiplus::RectF((REAL)(rcStatus_.left), (REAL)rcStatus_.top,
                            (REAL)(rcStatus_.right - rcStatus_.left - 12),
                            (REAL)(rcStatus_.bottom - rcStatus_.top)),
             2, 1, false, false);
}

// 视图选择逻辑（与 _raise_view 严格同序）
void App::paintViewBox(Graphics& g) {
    RECT box = rcView_;
    if (!core_->in_net()) {
        paintNetgateView(g, box);
        return;
    }
    if (currentGid_.empty()) {
        paintEmptyView(g, box);
        return;
    }
    if (chip_ == "chat") paintChatView(g, box);
    else if (chip_ == "files") paintFilesView(g, box);
    else paintMembersView(g, box);
}

void App::paintEmptyView(Graphics& g, const RECT& box) {
    int cx = (box.left + box.right) / 2;
    int cy = box.top + (int)((box.bottom - box.top) * 0.42);
    int y = cy - 110;
    drawText(g, L"\U0001F4AC", emojiFontPt(46), C_TEXT,
             Gdiplus::RectF((REAL)(cx - 60), (REAL)y, 120, 60), 1, 1, false, false);
    y += 60 + 8;
    drawText(g, L"选择左侧群组开始聊天", fontPt(13, false), C_SUB,
             Gdiplus::RectF((REAL)(cx - 150), (REAL)y, 300, (REAL)g_textH(13, false)), 1, 1, false,
             false);
    y += g_textH(13, false) + 4;
    drawText(g, L"或创建一个群组，邀请同学加入", fontPt(10, false), C_SUB,
             Gdiplus::RectF((REAL)(cx - 150), (REAL)y, 300, (REAL)g_textH(10, false)), 1, 1, false,
             false);
    y += g_textH(10, false) + 14;
    int bw = g_textW(L"＋ 创建群组", 11, true) + 44;
    int bh = g_textH(11, true) + 12;
    RECT br{cx - bw / 2, y, cx - bw / 2 + bw, y + bh};
    drawBtn(g, br, L"＋ 创建群组", 11, true, true, false, hoverAct_ == A_CREATE_GROUP, C_ACCENT,
            C_WHITE);
    Hot h;
    h.rc = br;
    h.act = A_CREATE_GROUP;
    hits_.push_back(h);
}

void App::paintNetgateView(Graphics& g, const RECT& box) {
    int cx = (box.left + box.right) / 2;
    int cy = box.top + (int)((box.bottom - box.top) * 0.42);
    int y = cy - 130;
    drawText(g, L"\U0001F4E1", emojiFontPt(44), C_TEXT,
             Gdiplus::RectF((REAL)(cx - 60), (REAL)y, 120, 60), 1, 1, false, false);
    y += 60 + 8;
    bool applying = core_->net_applying.isObj();
    std::wstring head = applying ? L"已提交入网申请，等待成员投票…" : L"正在扫描附近的内网…";
    drawText(g, head, fontPt(13, true), C_TEXT,
             Gdiplus::RectF((REAL)(cx - 260), (REAL)y, 520, (REAL)g_textH(13, true)), 1, 1, false,
             false);
    y += g_textH(13, true) + 4;
    std::wstring target = applying ? U8(core_->net_applying.get("name").asStr("")) : std::wstring();
    std::wstring detail = target.empty()
                              ? L"发现附近内网会自动申请加入；没有则自动建立自己的内网"
                              : (L"目标内网「" + target + L"」· 投票 1 天内有效");
    drawText(g, detail, fontPt(10, false), C_SUB,
             Gdiplus::RectF((REAL)(cx - 260), (REAL)y, 520, (REAL)g_textH(10, false)), 1, 1, false,
             false);
    y += g_textH(10, false);
    std::vector<nw::Json> cards = core_->net_open_votes();
    if (!cards.empty()) {
        y += 10;
        wchar_t buf[128];
        swprintf(buf, 128, L"当前投票：%d 项（左侧「投票」图标可查看）", (int)cards.size());
        drawText(g, buf, fontPt(10, true), C_BADGE,
                 Gdiplus::RectF((REAL)(cx - 260), (REAL)y, 520, (REAL)g_textH(10, true)), 1, 1,
                 false, false);
        y += g_textH(10, true);
    }
    y += 14;
    int bw = g_textW(L"打开内网页面", 11, true) + 44;
    int bh = g_textH(11, true) + 12;
    RECT br{cx - bw / 2, y, cx - bw / 2 + bw, y + bh};
    drawBtn(g, br, L"打开内网页面", 11, true, true, false, false, C_ACCENT, C_WHITE);
    Hot h;
    h.rc = br;
    h.act = A_NAV_NET;
    hits_.push_back(h);
}

void App::paintOverlay(Graphics& g, int w, int h) {
    if (!overlayOn_) return;
    int ow = 320;
    int oh = 78;
    int ox = w - ow - 16;
    int oy = 12;
    Gdiplus::RectF r((REAL)ox, (REAL)oy, (REAL)ow, (REAL)oh);
    fillRect(g, r, C_WHITE);
    {
        Pen pen(C_BORDER, 1.f);
        g.DrawRectangle(&pen, (REAL)ox + 0.5f, (REAL)oy + 0.5f, (REAL)ow - 1.f, (REAL)oh - 1.f);
    }
    drawText(g, overlayTitle_, fontPt(10, true), C_TEXT,
             Gdiplus::RectF((REAL)(ox + 12), (REAL)(oy + 8), (REAL)(ow - 24), 18), 0, 1, false,
             true);
    std::vector<std::wstring> lines =
        wrapText(measureG(), overlayMsg_, fontPt(9, false), (float)(ow - 24));
    int ly = oy + 30;
    for (size_t i = 0; i < lines.size() && i < 2; ++i) {
        drawText(g, lines[i], fontPt(9, false), C_SUB,
                 Gdiplus::RectF((REAL)(ox + 12), (REAL)ly, (REAL)(ow - 24), 16), 0, 1, false, true);
        ly += 16;
    }
    Hot hot;
    hot.rc = RECT{ox, oy, ox + ow, oy + oh};
    hot.act = A_NOTIFY_CLOSE;
    hits_.push_back(hot);
}

// ---------------------------------------------------------------- 命中 --
int App::hitAction(int x, int y, Hot* out) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (it->act == A_NONE || it->act == A_NOTIFY_CLOSE) continue;
        if (ptIn(it->rc, x, y)) {
            if (out) *out = *it;
            return it->act;
        }
    }
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (it->act == A_NOTIFY_CLOSE && ptIn(it->rc, x, y)) {
            if (out) *out = *it;
            return it->act;
        }
    }
    return A_NONE;
}

Hot* App::findHot(int x, int y, int act) {
    for (auto& h : hits_) {
        if (h.act == act && ptIn(h.rc, x, y)) return &h;
    }
    return nullptr;
}

// ---------------------------------------------------------------- 输入 --
void App::onLeftDown(int x, int y) {
    ensureHits();
    for (size_t i = 0; i < tracks_.size(); ++i) {
        RECT t = tracks_[i].rc;
        if (ptIn(t, x, y)) {
            ScrollArea* a = tracks_[i].area;
            int th = scrollThumbH(t, a->contentH, a->viewH());
            int tt = scrollThumbTop(t, a->contentH, a->viewH(), a->offset);
            if (y >= tt && y < tt + th) {
                dragTrack_ = (int)i;
                dragStartY_ = y;
                dragStartOff_ = a->offset;
            } else {
                int page = (std::max)(20, a->viewH() - 6);
                a->offset += (y < tt) ? -page : page;
                a->clamp();
            }
            invalidate();
            return;
        }
    }
    Hot h;
    int act = hitAction(x, y, &h);
    pressedAct_ = act;
    pressedArg_ = h.arg;
    pressedVal_ = h.val;
    // 焦点转移：点击非输入框区域时，搜索框失焦（内容为空则恢复占位态）
    if (act != A_SEARCH_FOCUS && act != A_CHAT_ENTRY) {
        if (search_.focused) {
            search_.focused = false;
            if (search_.text.empty()) {
                searchPh_ = true;
                search_.setPlaceholder(L"\U0001F50D  搜索群组");
                refreshGroupsList();
            }
            if (activeInput_ == &search_) activeInput_ = nullptr;
        }
        if (chatEntry_.focused && act != A_SEND_CHAT) {
            chatEntry_.focused = false;
            if (activeInput_ == &chatEntry_) activeInput_ = nullptr;
        }
    }
    if (act == A_NOTIFY_CLOSE) {
        overlayOn_ = false;
        if (overlayTimer_) { cancelTimer(overlayTimer_); overlayTimer_ = 0; }
        invalidate();
        return;
    }
    if (act == A_SEARCH_FOCUS) {
        searchPh_ = false;
        search_.clearPlaceholder();
        search_.focused = true;
        chatEntry_.focused = false;
        activeInput_ = &search_;
        invalidate();
        return;
    }
    if (act == A_CHAT_ENTRY) {
        chatEntry_.focused = true;
        search_.focused = false;
        activeInput_ = &chatEntry_;
        invalidate();
        return;
    }
    if (act == A_MEMBER_CARD) {
        openDm(h.arg);
        return;
    }
    if (act == A_GROUP_CARD) {
        selectGid(h.arg);
        return;
    }
    if (act == A_FILE_ROW) {
        tFiles_.sel = tFiles_.rowAt(x, y);
        selectedFileId_ = (tFiles_.sel >= 0 && tFiles_.sel < (int)tFiles_.iids.size())
                              ? tFiles_.iids[(size_t)tFiles_.sel]
                              : std::string();
        invalidate();
        return;
    }
    if (act == A_HIST_ROW) {
        tHist_.sel = tHist_.rowAt(x, y);
        selectedHistId_ = (tHist_.sel >= 0 && tHist_.sel < (int)tHist_.iids.size())
                              ? tHist_.iids[(size_t)tHist_.sel]
                              : std::string();
        invalidate();
        return;
    }
    if (act == A_NEARBY_ROW) {
        tNearby_.sel = tNearby_.rowAt(x, y);
        invalidate();
        return;
    }
    if (act == A_NETMEMBER_ROW) {
        tNetMembers_.sel = tNetMembers_.rowAt(x, y);
        invalidate();
        return;
    }
    invalidate();
}

void App::onLeftUp(int x, int y) {
    if (dragTrack_ >= 0) {
        dragTrack_ = -1;
        return;
    }
    ensureHits();
    Hot h;
    int act = hitAction(x, y, &h);
    int pact = pressedAct_;
    std::string parg = pressedArg_;
    int pval = pressedVal_;
    pressedAct_ = A_NONE;
    if (act == A_NONE || act != pact || h.arg != parg || h.val != pval) {
        invalidate();
        return;
    }
    switch (act) {
    case A_NAV_CHAT:
    case A_NAV_NET:
    case A_NAV_VOTE:
    case A_NAV_MORE:
    case A_NAV_LOG:
        switchNav(std::string(kNavKey[act - A_NAV_CHAT]));
        break;
    case A_MY_AVATAR:
    case A_SETTINGS_BTN:
        openSettings();
        break;
    case A_CREATE_GROUP:
        openCreateGroup();
        break;
    case A_JOIN_GROUP:
        openJoinGroup();
        break;
    case A_CHIP_CHAT:
    case A_CHIP_FILES:
    case A_CHIP_MEMBERS:
        setChip(kChipKey[act - A_CHIP_CHAT]);
        break;
    case A_LEAVE_GROUP:
        if (!currentGid_.empty()) leaveGroup(currentGid_);
        break;
    case A_SEND_CHAT:
        sendChat();
        break;
    case A_SEND_FILE:
        sendFiles();
        break;
    case A_FILE_OPEN:
        openSelectedFile();
        break;
    case A_FILE_RETRY:
        retryDownload();
        break;
    case A_FILE_OPENDIR:
        openDownloadsDir();
        break;
    case A_NET_SCAN:
        netScan();
        break;
    case A_NET_CREATE:
        netCreate();
        break;
    case A_NET_RENAME:
        netRename();
        break;
    case A_NET_LEAVE:
        netLeave();
        break;
    case A_NET_APPLY:
        netApplySelected();
        break;
    case A_VOTE_CAST:
        castVote(parg, pval);
        break;
    case A_VOTE_CANCEL_APPLY:
        onNetCancelApply();
        break;
    case A_VOTE_PAGE_OPEN:
        hideVotePopup();
        switchNav("vote");
        break;
    case A_CHANGE_VOTE:
        openChangeVoteFromHistory();
        break;
    case A_TOOL_DATADIR:
        openDataDir();
        break;
    case A_TOOL_UPDATE:
        onManualUpdate();
        break;
    case A_TOOL_SETTINGS:
        openSettings();
        break;
    case A_CLEAR_LOG:
        clearLog();
        break;
    default:
        break;
    }
    invalidate();
}

void App::onMouseMove(int x, int y) {
    if (dragTrack_ >= 0 && (GetKeyState(VK_LBUTTON) & 0x8000)) {
        Track& t = tracks_[(size_t)dragTrack_];
        ScrollArea* a = t.area;
        int th = scrollThumbH(t.rc, a->contentH, a->viewH());
        int span = (t.rc.bottom - t.rc.top) - th;
        int maxOff = a->maxOffset();
        if (span > 0 && maxOff > 0) {
            int dy = y - dragStartY_;
            a->offset = dragStartOff_ + (int)((double)dy * (double)maxOff / (double)span);
            a->clamp();
        }
        invalidate();
        return;
    }
    ensureHits();
    Hot h;
    int act = hitAction(x, y, &h);
    bool changed = (act != hoverAct_ || h.arg != hoverArg_ || h.val != hoverVal_);
    hoverAct_ = act;
    hoverArg_ = h.arg;
    hoverVal_ = h.val;
    for (ScrollArea* a : scrolls_) {
        bool hv = a->contains(x, y);
        if (hv != a->hover) { a->hover = hv; changed = true; }
    }
    bool onVoteBtn = ptIn(rcNav_[2], x, y);
    if (onVoteBtn && !mouseOnVoteBtn_) scheduleVotePopupShow();
    if (!onVoteBtn && mouseOnVoteBtn_) scheduleVotePopupHide();
    mouseOnVoteBtn_ = onVoteBtn;
    if (changed) invalidate();
}

void App::onMouseWheel(int x, int y, int delta) {
    int notches = -delta / WHEEL_DELTA;
    for (ScrollArea* a : scrolls_) {
        if (!a->contains(x, y)) continue;
        int unit = (std::max)(20, a->viewH() / 10);      // Tk canvas 滚轮单位 ≈ 1/10 视高
        a->offset += notches * unit;
        a->clamp();
        invalidate();
        return;
    }
    for (Table* t : tables_) {
        if (!t || !ptIn(t->rowsRect(), x, y)) continue;
        t->offset += notches * t->rowH * 3;
        t->clamp();
        invalidate();
        return;
    }
}

void App::onKeyDown(WPARAM k) {
    TextInput* in = activeInput_;
    if (!in || !in->focused) return;
    switch (k) {
    case VK_BACK:
        if (in->caret == 0) return;
        in->backspace();
        break;
    case VK_DELETE: in->del(); break;
    case VK_LEFT: in->moveLeft(); break;
    case VK_RIGHT: in->moveRight(); break;
    case VK_HOME: in->home(); break;
    case VK_END: in->end(); break;
    case VK_RETURN:
        if (in == &chatEntry_) sendChat();
        break;
    default:
        return;
    }
    invalidate();
}

void App::onChar(wchar_t c) {
    TextInput* in = activeInput_;
    if (!in || !in->focused) return;
    if (c == 8) {
        if (in->caret == 0) return;
        in->backspace();
    } else {
        in->insertChar(c);
    }
    invalidate();
}

// ---------------------------------------------------------------- 事件泵 --
void App::pollEvents() {
    nw::Event ev;
    int guard = 0;
    while (guard++ < 5000 && events_->pop(ev, 0)) {
        const std::string& k = ev.kind;
        const nw::Json& a = ev.args;
        if (k == "chat") {
            if (a.size() > 0) onEvChat(a.at(0));
        } else if (k == "log") {
            if (a.size() > 0) appendLog(U8(a.at(0).asStr("")));
        } else if (k == "syslog") {
            if (a.size() > 0) appendLog(nowStamp() + L" 系统: " + U8(a.at(0).asStr("")));
        } else if (k == "syschat") {
            onEvSyschat(a);
        } else if (k == "peers_changed") {
            refreshPeers();
        } else if (k == "groups_changed") {
            refreshGroupsList();
        } else if (k == "name_changed") {
            if (a.size() > 0) applyMyNameUi();
        } else if (k == "file_new") {
            if (a.size() > 0) onEvFileNew(a.at(0));
        } else if (k == "file_status") {
            if (a.size() > 0) onEvFileStatus(a.at(0));
        } else if (k == "notify") {
            if (a.size() >= 2) notify(U8(a.at(0).asStr("")), U8(a.at(1).asStr("")));
        } else if (k == "update") {
            if (a.size() > 0) onEvUpdate(a.at(0));
        } else if (k == "rename_request") {
            if (a.size() > 0) onRenameRequest(a.at(0));
        } else if (k == "rename_vote_progress") {
            if (a.size() >= 3)
                onSettingsVoteProgress(a.at(0).asStr(""), (int)a.at(1).asInt(0),
                                       (int)a.at(2).asInt(0));
        } else if (k == "rename_vote_done") {
            if (a.size() >= 4)
                onSettingsVoteDone(a.at(0).asStr(""), a.at(1).asBool(false), U8(a.at(2).asStr("")),
                                   U8(a.at(3).asStr("")));
        } else if (k == "dm") {
            if (a.size() > 0) onEvDm(a.at(0));
        } else if (k == "dm_sent") {
            if (a.size() > 0) onEvDmSent(a.at(0));
        } else if (k == "net_changed") {
            onNetChanged();
        } else if (k == "net_vote_changed") {
            onNetVoteChanged();
        } else if (k == "net_status") {
            if (a.size() > 0) {
                std::wstring t = U8(a.at(0).asStr(""));
                netStatusText_ = leftChars(t, 80);
                appendLog(nowStamp() + L" 内网: " + t);
                invalidate();
            }
        } else if (k == "net_nearby_changed") {
            if (nav_ == "net") renderNetPage();
        } else if (k == "net_result") {
            onNetResult(a.size() > 0 && a.at(0).asBool(false),
                        a.size() > 1 ? U8(a.at(1).asStr("")) : std::wstring());
        } else if (k == "act_log") {
            if (a.size() > 0 && a.at(0).isArr()) {
                for (auto& x : a.at(0).items()) appendLog(U8(x.asStr("")));
            }
        }
    }
}

} // namespace ui
