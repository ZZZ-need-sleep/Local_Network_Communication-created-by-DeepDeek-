// forms.cpp —— 自绘对话框工具集与全部对话框 / 私聊窗口（无任何标准控件）
#include "forms.h"

#include "app.h"

#include "../common/util.h"

#include <cstdio>

using namespace Gdiplus;

namespace ui {

int g_textH(int pt, bool bold);
int g_textW(const std::wstring& s, int pt, bool bold);

static const wchar_t* kFormClass = L"NwForm";
static bool g_formClassReg = false;

// ---------------------------------------------------------------- Form --
Form::~Form() {}

int Form::textH(int pt, bool bold) { return g_textH(pt, bold); }
int Form::textW(const std::wstring& s, int pt, bool bold) { return g_textW(s, pt, bold); }

FItem& Form::addLabel(const std::wstring& text, int x, int y, int w, int h, int pt, bool bold,
                      const Gdiplus::Color& fg, int align, bool wrap) {
    FItem it;
    it.kind = FItem::Label;
    it.rc = RECT{x, y, x + w, y + h};
    it.text = text;
    it.pt = pt;
    it.bold = bold;
    it.fg = fg;
    it.align = align;
    it.wrap = wrap;
    it.wrapW = w;
    items_.push_back(it);
    return items_.back();
}

FItem& Form::addButton(int id, const std::wstring& text, int x, int y, int w, int h, int pt,
                       bool bold, bool primary, bool outline) {
    FItem it;
    it.kind = FItem::Button;
    it.id = id;
    it.rc = RECT{x, y, x + w, y + h};
    it.text = text;
    it.pt = pt;
    it.bold = bold;
    it.primary = primary;
    it.outline = outline;
    it.flat = !primary && !outline;
    it.fg = primary ? C_WHITE : (outline ? C_TEXT : C_SUB);
    it.bg = primary ? C_ACCENT : C_PANEL;
    items_.push_back(it);
    return items_.back();
}

FItem& Form::addInput(TextInput* in, int x, int y, int w, int h, int pt) {
    FItem it;
    it.kind = FItem::Input;
    it.rc = RECT{x, y, x + w, y + h};
    it.pt = pt;
    it.input = in;
    it.bg = C_BG;                 // 输入框硬编码 #f7f7f7
    it.fg = C_TEXT;
    items_.push_back(it);
    return items_.back();
}

FItem& Form::addTable(Table* t, int x, int y, int w, int h) {
    FItem it;
    it.kind = FItem::TableItem;
    it.rc = RECT{x, y, x + w, y + h};
    it.table = t;
    t->rc = it.rc;
    items_.push_back(it);
    return items_.back();
}

FItem& Form::addLine(int x, int y, int w) {
    FItem it;
    it.kind = FItem::Line;
    it.rc = RECT{x, y, x + w, y + 1};
    items_.push_back(it);
    return items_.back();
}

void Form::invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

UINT_PTR Form::post(std::function<void()> fn, int ms) {
    UINT_PTR id = nextTimer_++;
    timers_[id] = std::move(fn);
    SetTimer(hwnd_, id, (UINT)ms, nullptr);
    return id;
}

void Form::cancelTimer(UINT_PTR id) {
    if (!id) return;
    KillTimer(hwnd_, id);
    timers_.erase(id);
}

bool Form::createForm(HINSTANCE inst, HWND owner, App* app, const std::wstring& title, int w,
                      int h, bool resizable) {
    inst_ = inst;
    owner_ = owner;
    app_ = app;
    clientW_ = w;
    clientH_ = h;
    if (!g_formClassReg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &Form::sWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kFormClass;
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);
        g_formClassReg = true;
    }
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    RECT r{0, 0, w, h};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    hwnd_ = CreateWindowExW(0, kFormClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, owner, nullptr, inst, this);
    if (!hwnd_) return false;
    SetTimer(hwnd_, 3, 530, nullptr);       // 光标闪烁
    onCreated();
    return true;
}

void Form::close(int result) {
    result_ = result;
    if (hwnd_) DestroyWindow(hwnd_);
}

int Form::runModal() {
    if (!hwnd_) return 0;
    if (owner_) EnableWindow(owner_, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    MSG msg;
    while (IsWindow(hwnd_) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner_) {
        EnableWindow(owner_, TRUE);
        SetForegroundWindow(owner_);
    }
    return result_;
}

LRESULT CALLBACK Form::sWndProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    Form* self = (Form*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        self = (Form*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->hwnd_ = hwnd;
    }
    if (self) return self->wndProc(hwnd, m, w, l);
    return DefWindowProcW(hwnd, m, w, l);
}

FItem* Form::itemAt(int x, int y) {
    for (auto& it : items_) {
        if (it.hidden) continue;
        if (ptIn(it.rc, x, y)) return &it;
    }
    return nullptr;
}

FItem* Form::itemById(int id) {
    for (auto& it : items_) {
        if (it.id == id) return &it;
    }
    return nullptr;
}

void Form::paintInput(Graphics& g, FItem& it) {
    Gdiplus::RectF r((REAL)it.rc.left, (REAL)it.rc.top, (REAL)(it.rc.right - it.rc.left),
                     (REAL)(it.rc.bottom - it.rc.top));
    fillRect(g, r, it.bg);
    TextInput* in = it.input;
    if (!in) return;
    Gdiplus::Font* f = fontPt(it.pt, false);
    int tx = it.rc.left + 6;
    std::wstring s = in->placeholder ? in->phText : in->text;
    Gdiplus::Color c = in->placeholder ? C_SUB : it.fg;
    drawText(g, s, f, c,
             Gdiplus::RectF((REAL)tx, (REAL)it.rc.top, (REAL)(it.rc.right - tx - 6),
                            (REAL)(it.rc.bottom - it.rc.top)),
             0, 1, false, !in->placeholder);
    if (focus_ == in && caretOn_) {
        int cx = in->caretX(measureG(), f, tx);
        if (cx < it.rc.left + 2) cx = it.rc.left + 2;
        drawVDivider(g, cx, it.rc.top + 3, it.rc.bottom - 3, C_TEXT);
    }
}

void Form::paintItems(Graphics& g, int w, int h) {
    fillRect(g, Gdiplus::RectF(0, 0, (REAL)w, (REAL)h), C_PANEL);
    for (auto& it : items_) {
        if (it.hidden) continue;
        switch (it.kind) {
        case FItem::Label: {
            Gdiplus::RectF r((REAL)it.rc.left, (REAL)it.rc.top, (REAL)(it.rc.right - it.rc.left),
                             (REAL)(it.rc.bottom - it.rc.top));
            if (it.wrap) {
                std::vector<std::wstring> lines =
                    wrapText(measureG(), it.text, fontPt(it.pt, it.bold),
                             (float)(it.wrapW > 0 ? it.wrapW : (it.rc.right - it.rc.left)));
                int y = it.rc.top;
                for (auto& ln : lines) {
                    drawText(g, ln, fontPt(it.pt, it.bold), it.fg,
                             Gdiplus::RectF((REAL)it.rc.left, (REAL)y,
                                            (REAL)(it.rc.right - it.rc.left),
                                            (REAL)textH(it.pt, it.bold)),
                             it.align, 1, false, false);
                    y += textH(it.pt, it.bold);
                }
            } else {
                drawText(g, it.text, fontPt(it.pt, it.bold), it.fg, r, it.align, it.valign, false,
                         it.ellipsis);
            }
            break;
        }
        case FItem::Button: {
            Gdiplus::RectF r((REAL)it.rc.left, (REAL)it.rc.top, (REAL)(it.rc.right - it.rc.left),
                             (REAL)(it.rc.bottom - it.rc.top));
            bool hov = (hoverId_ == it.id) && !it.disabled;
            if (it.primary) {
                fillRect(g, r, it.disabled ? RGBc(0xa8dfc0) : (hov ? C_ACCENT_HV : C_ACCENT));
            } else {
                fillRect(g, r, hov ? C_HOVER : it.bg);
                if (it.outline) {
                    Pen pen(it.disabled ? RGBc(0xeeeeee) : C_BORDER, 1.f);
                    g.DrawRectangle(&pen, (REAL)it.rc.left + 0.5f, (REAL)it.rc.top + 0.5f,
                                    (REAL)(it.rc.right - it.rc.left) - 1.f,
                                    (REAL)(it.rc.bottom - it.rc.top) - 1.f);
                }
            }
            drawText(g, it.text, fontPt(it.pt, it.bold), it.disabled ? C_SUB : it.fg, r, 1, 1,
                     false, false);
            break;
        }
        case FItem::Input:
            paintInput(g, it);
            break;
        case FItem::TableItem:
            if (it.table) drawTable(g, *it.table);
            break;
        case FItem::Line:
            drawHDivider(g, it.rc.left, it.rc.top, it.rc.right, C_BORDER);
            break;
        default:
            break;
        }
    }
}

void Form::relayout() {
    items_.clear();
    buildLayout();
}

LRESULT Form::wndProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr;
        GetClientRect(hwnd, &cr);
        DblBuffer db;
        if (db.begin(hdc, cr.right, cr.bottom)) {
            Graphics g(db.mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            relayout();
            paintItems(g, cr.right, cr.bottom);
            onPaintExtra(g, cr.right, cr.bottom);
            db.end();
            db.release();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        relayout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_TIMER: {
        if (w == 3) {
            caretOn_ = !caretOn_;
            if (focus_) invalidate();
            return 0;
        }
        auto it = timers_.find((UINT_PTR)w);
        if (it == timers_.end()) return 0;
        std::function<void()> fn = it->second;
        timers_.erase(it);
        KillTimer(hwnd, (UINT_PTR)w);
        fn();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) onActivated();
        return 0;
    case WM_SETFOCUS:
        onActivated();
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        relayout();
        FItem* it = itemAt(x, y);
        if (it && it->kind == FItem::Input) {
            focus_ = it->input;
            if (focus_ && focus_->placeholder) focus_->clearPlaceholder();
            invalidate();
            return 0;
        }
        if (it && it->kind == FItem::TableItem && it->table) {
            it->table->sel = it->table->rowAt(x, y);
            selRow_ = it->table->sel;
            invalidate();
            return 0;
        }
        pressedId_ = (it && it->kind == FItem::Button && !it->disabled) ? it->id : 0;
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (!pressedId_) return 0;
        int pid = pressedId_;
        pressedId_ = 0;
        relayout();
        FItem* it = itemAt(x, y);
        if (it && it->kind == FItem::Button && it->id == pid && !it->disabled) onClick(pid, 0);
        invalidate();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        relayout();
        FItem* it = itemAt(x, y);
        if (it && it->kind == FItem::TableItem && it->table) {
            it->table->sel = it->table->rowAt(x, y);
            onDblClick(it->id, it->table->sel);
        } else if (it) {
            onDblClick(it->id, -1);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        relayout();
        FItem* it = itemAt(x, y);
        int id = (it && it->kind == FItem::Button) ? it->id : 0;
        if (id != hoverId_) {
            hoverId_ = id;
            invalidate();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(hwnd, &p);
        int notches = -GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        if (onWheel(p.x, p.y, notches)) return 0;
        relayout();
        Table* t = nullptr;
        for (auto& i2 : items_) {
            if (i2.kind == FItem::TableItem && i2.table && ptIn(i2.rc, p.x, p.y)) t = i2.table;
        }
        if (t) {
            t->offset += notches * t->rowH * 3;
            t->clamp();
            invalidate();
        }
        return 0;
    }
    case WM_CHAR:
    case WM_IME_CHAR: {
        wchar_t c = (wchar_t)w;
        if (focus_) {
            if (c == 8) {
                if (focus_->caret > 0) focus_->backspace();
            } else if (c >= 32) {
                focus_->insertChar(c);
            }
            invalidate();
        } else {
            onCharInput(c);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        WPARAM k = w;
        if (focus_) {
            switch (k) {
            case VK_BACK:
                if (focus_->caret == 0) return 0;
                focus_->backspace();
                break;
            case VK_DELETE: focus_->del(); break;
            case VK_LEFT: focus_->moveLeft(); break;
            case VK_RIGHT: focus_->moveRight(); break;
            case VK_HOME: focus_->home(); break;
            case VK_END: focus_->end(); break;
            case VK_RETURN: onEnterKey(); break;
            default: return 0;
            }
            invalidate();
            return 0;
        }
        if (k == VK_RETURN || k == VK_SPACE) onEnterKey();
        return 0;
    }
    case WM_CLOSE:
        if (!onCloseRequest()) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        for (auto& kv : timers_) KillTimer(hwnd, kv.first);
        timers_.clear();
        hwnd_ = nullptr;
        onDestroyed();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// 按 owner 摆放窗口：mode 0 居中（纵向 /divY）、1 右下角、2 私聊
static void placeWindow(HWND hwnd, HWND owner, int w, int h, int mode, int divY) {
    RECT orc{0, 0, 1180, 740};
    if (owner) GetWindowRect(owner, &orc);
    int ow = orc.right - orc.left, oh = orc.bottom - orc.top;
    int x = 0, y = 0;
    if (mode == 0) {
        x = orc.left + (ow - w) / 2;
        y = orc.top + (oh - h) / divY;
    } else if (mode == 1) {
        x = orc.left + ow - w - 40;
        y = orc.top + oh - h - 60;
    } else {
        x = orc.left + (ow - w) / 2 + 90;
        y = orc.top + ((oh - h) / 2 - 20 > 0 ? (oh - h) / 2 - 20 : 0);
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Form::fitToClient(int w, int h) {
    clientW_ = w;
    clientH_ = h;
    RECT r{0, 0, w, h};
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(hwnd_, GWL_STYLE), FALSE, 0);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

void Form::centerOnOwner(int w, int h, int divY) {
    placeWindow(hwnd_, owner_, w, h, 0, divY);
}
void Form::centerRightBottom(int w, int h) { placeWindow(hwnd_, owner_, w, h, 1, 3); }
void Form::centerLikeChat(int w, int h) { placeWindow(hwnd_, owner_, w, h, 2, 2); }

void Form::showWindow() {
    if (!hwnd_) return;
    relayout();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void Form::setFocusInput(TextInput* t) {
    focus_ = t;
    if (t) t->focused = true;
}

// ------------------------------------------------------------ 消息框 --
namespace {

class MsgForm : public Form {
public:
    MsgForm(const std::wstring& title, const std::wstring& text, int kind, bool ask)
        : title_(title), text_(text), kind_(kind), ask_(ask) {}

    int calcHeight() {
        std::vector<std::wstring> lines = wrapText(measureG(), text_, fontPt(10, false), 340.f);
        int bodyH = (int)lines.size() * textH(10, false);
        return 20 + bodyH + 18 + (textH(10, true) + 12) + 20;
    }

protected:
    void buildLayout() override {
        int pad = 20;
        int w = clientW_;
        std::vector<std::wstring> lines = wrapText(measureG(), text_, fontPt(10, false), 340.f);
        int lh = textH(10, false);
        for (size_t i = 0; i < lines.size(); ++i) {
            addLabel(lines[i], pad, pad + (int)i * lh, w - 2 * pad, lh, 10, false, C_TEXT, 0,
                     false);
        }
        int by = pad + (int)lines.size() * lh + 18;
        int bh = textH(10, true) + 12;
        if (ask_) {
            int bw = textW(L"是", 10, true) + 44;
            addButton(2, L"否", w - pad - bw, by, bw, bh, 10, false, false, true);
            addButton(1, L"是", w - pad - bw - 10 - bw, by, bw, bh, 10, true, true, false);
        } else {
            int bw = textW(L"确定", 10, true) + 44;
            addButton(1, L"确定", w - pad - bw, by, bw, bh, 10, true, true, false);
        }
    }
    void onClick(int id, int val) override {
        (void)val;
        close(id);
    }
    void onCreated() override {
        int w = 380, h = calcHeight();
        fitToClient(w, h);
        centerOnOwner(w, h, 3);
    }

private:
    std::wstring title_, text_;
    int kind_;
    bool ask_;
};

class InputForm : public Form {
public:
    InputForm(const std::wstring& title, const std::wstring& prompt, const std::wstring& initial,
              const std::wstring& emptyMsg)
        : title_(title), prompt_(prompt), initial_(initial), emptyMsg_(emptyMsg) {}
    std::wstring value;

protected:
    void buildLayout() override {
        if (!inited_) {
            name_.setText(initial_);
            inited_ = true;
        }
        int pad = 22, w = clientW_;
        addLabel(title_, pad, 18, w - 2 * pad, textH(14, true), 14, true, C_TEXT, 0, false);
        addLabel(prompt_, pad, 18 + textH(14, true) + 12, w - 2 * pad, textH(9, false), 9, false,
                 C_SUB, 0, true);
        int iy = 18 + textH(14, true) + 12 + textH(9, false) + 6;
        int ih = textH(11, false) + 14;
        addInput(&name_, pad, iy, w - 2 * pad, ih, 11);
        int by = iy + ih + 20;
        int bw = textW(L"确定", 10, true) + 40;
        int bh = textH(10, true) + 12;
        addButton(2, L"取消", w - pad - bw, by, bw, bh, 10, false, false, true);
        addButton(1, L"确定", w - pad - bw - 10 - bw, by, bw, bh, 10, true, true, false);
    }
    void onClick(int id, int val) override {
        (void)val;
        if (id == 1) {
            value = trimW(name_.text);
            if (value.empty()) {
                msgBox(app_, L"提示", emptyMsg_, 0, hwnd_);
                return;
            }
            close(1);
        } else {
            close(0);
        }
    }
    void onEnterKey() override { onClick(1, 0); }
    void onCreated() override {
        int w = 380;
        int h = 18 + textH(14, true) + 12 + textH(9, false) + 6 + (textH(11, false) + 14) + 20 +
                (textH(10, true) + 12) + 20;
        fitToClient(w, h);
        centerOnOwner(w, h, 3);
        focus_ = &name_;
        name_.focused = true;
    }

private:
    std::wstring title_, prompt_, initial_, emptyMsg_;
    bool inited_ = false;
    TextInput name_;
};

} // namespace

void msgBox(App* app, const std::wstring& title, const std::wstring& text, int kind, HWND owner) {
    MsgForm* f = new MsgForm(title, text, kind, false);
    HWND own = owner ? owner : (app ? app->hwnd() : nullptr);
    HINSTANCE inst = app ? app->inst() : GetModuleHandleW(nullptr);
    if (!f->createForm(inst, own, app, title, 380, 180, false)) {
        MessageBoxW(own, text.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        delete f;
        return;
    }
    f->runModal();
    delete f;
}

bool askYesNo(App* app, const std::wstring& title, const std::wstring& text, HWND owner) {
    MsgForm* f = new MsgForm(title, text, 0, true);
    HWND own = owner ? owner : (app ? app->hwnd() : nullptr);
    HINSTANCE inst = app ? app->inst() : GetModuleHandleW(nullptr);
    if (!f->createForm(inst, own, app, title, 380, 180, false)) {
        bool r = MessageBoxW(own, text.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES;
        delete f;
        return r;
    }
    int r = f->runModal();
    delete f;
    return r == 1;
}

// --------------------------------------------------- 内网名称输入对话框 --
void netCreateDialog(App* app) {
    if (!app) return;
    std::wstring def = U8(nw::hostname_utf8()) + L" 的内网";
    InputForm* f = new InputForm(L"建立内网", L"给内网起个名字（如：高三（2）班内网）", def,
                                 L"名称不能为空");
    if (!f->createForm(app->inst(), app->hwnd(), app, L"建立内网", 380, 200, false)) {
        delete f;
        return;
    }
    int r = f->runModal();
    std::wstring name = f->value;
    delete f;
    if (r != 1) return;
    try {
        app->core()->net_create(nw::ws2u8(name));
    } catch (const std::exception& e) {
        msgBox(app, L"建立内网失败", U8(e.what()), 2, app->hwnd());
    } catch (...) {
        msgBox(app, L"建立内网失败", L"未知错误", 2, app->hwnd());
    }
    app->renderNetPage();
}

void netRenameDialog(App* app, const std::wstring& curName) {
    if (!app) return;
    InputForm* f = new InputForm(L"重命名内网", L"新的内网名称：", curName, L"名称不能为空");
    if (!f->createForm(app->inst(), app->hwnd(), app, L"重命名内网", 380, 200, false)) {
        delete f;
        return;
    }
    int r = f->runModal();
    std::wstring name = f->value;
    delete f;
    if (r != 1 || name.empty()) return;
    try {
        app->core()->net_rename(nw::ws2u8(name));
    } catch (const std::exception& e) {
        msgBox(app, L"重命名失败", U8(e.what()), 2, app->hwnd());
    } catch (...) {
        msgBox(app, L"重命名失败", L"未知错误", 2, app->hwnd());
    }
    app->renderNetPage();
}

// --------------------------------------------------- 创建群组对话框 --
namespace {

class CreateGroupForm : public Form {
public:
    explicit CreateGroupForm(App* app) : App_(app) {}

protected:
    void buildLayout() override {
        const int pad = 22;
        int w = 360;
        addLabel(L"创建群组", pad, 20, w - 2 * pad, textH(14, true), 14, true, C_TEXT, 0, false);
        int y = 20 + textH(14, true) + 10;
        addLabel(L"群组名称（如：高三（2）班）", pad, y, w - 2 * pad, textH(9, false), 9, false,
                 C_SUB, 0, false);
        y += textH(9, false) + 4;
        int ih = textH(11, false) + 14;
        addInput(&name_, pad, y, w - 2 * pad, ih, 11);
        y += ih + 6;
        addLabel(L"创建后自动生成群号，其他同学加入群组时输入该群号即可", pad, y, 310,
                 textH(8, false) * 2, 8, false, C_SUB, 0, true);
        y += textH(8, false) * 2 + 14;
        int bh = textH(10, true) + 10;
        int bw = textW(L"0000000000", 10, false) + 6;
        addButton(2, L"取消", w - pad - textW(L"取消", 10, false) - 8, y,
                  textW(L"取消", 10, false) + 8, bh, 10, false, false, false);
        addButton(1, L"创建", w - pad - (textW(L"取消", 10, false) + 8) - 10 - bw, y, bw, bh, 10,
                  true, true, false);
    }
    void onClick(int id, int val) override {
        (void)val;
        if (id == 2) {
            close(0);
            return;
        }
        std::wstring name = trimW(name_.text);
        if (name.empty()) {
            msgBox(App_, L"提示", L"请输入群组名称", 0, hwnd_);
            return;
        }
        try {
            nw::Json g = App_->core()->create_group(nw::ws2u8(name));
            App_->refreshGroupsList();
            App_->selectGid(g.get("gid").asStr(""));
            close(1);
        } catch (const std::exception& e) {
            msgBox(App_, L"提示", U8(e.what()), 0, hwnd_);
            App_->switchNav("net");
        } catch (...) {
            msgBox(App_, L"提示", L"创建群组失败", 0, hwnd_);
        }
    }
    void onEnterKey() override { onClick(1, 0); }
    void onCreated() override {
        int w = 360, h = 210;
        fitToClient(w, h);
        centerOnOwner(w, h, 3);
        focus_ = &name_;
        name_.focused = true;
    }

private:
    App* App_ = nullptr;
    TextInput name_;
};

// --------------------------------------------------- 加入群组对话框 --
class JoinGroupForm : public Form {
public:
    explicit JoinGroupForm(App* app) : App_(app) {}

protected:
    void buildLayout() override {
        const int pad = 20;
        int w = clientW_;
        addLabel(L"加入群组", pad, 16, w - 2 * pad, textH(14, true), 14, true, C_TEXT, 0, false);
        int y = 16 + textH(14, true) + 4;
        addLabel(L"从局域网发现的群组（自动扫描，双击加入）", pad, y, w - 2 * pad,
                 textH(9, false), 9, false, C_SUB, 0, false);
        y += textH(9, false) + 8;
        int tableH = 28 + 8 * 30;
        table_.rowH = 30;
        table_.withScrollbar = true;
        addTable(&table_, pad, y, w - 2 * pad, tableH);
        y += tableH + 8;
        int bh = textH(10, true) + 8;
        int b1 = textW(L"加入所选群组", 10, true) + 28;
        addButton(1, L"加入所选群组", pad, y, b1, bh, 10, true, true, false);
        addButton(2, L"刷新", pad + b1 + 8, y, textW(L"刷新", 10, false) + 20, bh, 10, false,
                  false, false);
        y += bh + 10;
        addLine(pad, y, w - 2 * pad);
        y += 10;
        addLabel(L"或者手动输入群号加入：", pad, y, w - 2 * pad, textH(9, false), 9, false, C_SUB,
                 0, false);
        y += textH(9, false) + 4;
        int ih = textH(10, false) + 8;
        int lw1 = textW(L"群号", 10, false);
        int lw2 = textW(L"群名（可留空）", 10, false);
        int ix = pad + lw1 + 6;
        addLabel(L"群号", pad, y, lw1, ih, 10, false, C_TEXT, 0, false);
        addInput(&gid_, ix, y, textW(L"000000000000", 10, false) + 12, ih, 10);
        int ix2 = ix + textW(L"000000000000", 10, false) + 12 + 6;
        addLabel(L"群名（可留空）", ix2, y, lw2, ih, 10, false, C_TEXT, 0, false);
        int ix3 = ix2 + lw2 + 6;
        addInput(&gname_, ix3, y, textW(L"0000000000000000", 10, false) + 12, ih, 10);
        addButton(3, L"加入", ix3 + textW(L"0000000000000000", 10, false) + 12 + 4, y,
                  textW(L"加入", 10, true) + 24, ih, 10, true, true, false);
    }
    void refreshTable() {
        table_.headers = {L"群名", L"群号", L"群主电脑", L"在线人数"};
        table_.widths = {150, 90, 130, 80};
        table_.rows.clear();
        table_.iids.clear();
        std::vector<std::pair<std::wstring, std::string>> list;
        for (auto& kv : App_->core()->discovered) {
            list.push_back({U8(kv.second.get("gname").asStr("")), kv.first});
        }
        std::sort(list.begin(), list.end());
        double now = nw::now_ts();
        for (auto& p : list) {
            auto it = App_->core()->discovered.find(p.second);
            if (it == App_->core()->discovered.end()) continue;
            std::string gid = p.second;
            int online = 0;
            for (auto& pkv : App_->core()->peers) {
                if (now - (pkv.second.ts > 0 ? pkv.second.ts : 0) > 30.0) continue;
                for (auto& g : pkv.second.groups.items()) {
                    if (g.get("gid").asStr("") == gid) { ++online; break; }
                }
            }
            std::vector<std::wstring> row;
            row.push_back(U8(it->second.get("gname").asStr("")));
            row.push_back(U8(gid));
            row.push_back(U8(it->second.get("host_name").asStr("")));
            row.push_back(std::to_wstring(online));
            table_.rows.push_back(row);
            table_.iids.push_back(gid);
        }
    }
    void doJoin() {
        if (table_.sel < 0 || table_.sel >= (int)table_.iids.size()) return;
        std::string gid = table_.iids[(size_t)table_.sel];
        std::wstring gname = table_.rows[(size_t)table_.sel][0];
        try {
            App_->core()->join_group(gid, nw::ws2u8(gname));
        } catch (...) {
            return;
        }
        App_->refreshGroupsList();
        App_->selectGid(gid);
        close(1);
    }
    void doManual() {
        std::wstring gid = trimW(gid_.text);
        std::wstring gname = trimW(gname_.text);
        if (gid.empty()) {
            msgBox(App_, L"提示", L"请输入群号（向创建者索取）", 0, hwnd_);
            return;
        }
        try {
            App_->core()->join_group(nw::ws2u8(gid), nw::ws2u8(gname));
        } catch (...) {
            return;
        }
        App_->refreshGroupsList();
        App_->selectGid(nw::to_lower_ascii(nw::ws2u8(gid)));
        close(1);
    }
    void onClick(int id, int val) override {
        (void)val;
        if (id == 1) doJoin();
        else if (id == 2) {
            refreshTable();
            invalidate();
        } else if (id == 3) {
            doManual();
        }
    }
    void onDblClick(int id, int val) override {
        (void)id;
        if (val >= 0) doJoin();
    }
    void onCreated() override {
        int w = 560, h = 440;
        fitToClient(w, h);
        centerOnOwner(w, h, 3);
        refreshTable();
        post([this]() {
            refreshTable();
            invalidate();
            post([this]() {
                refreshTable();
                invalidate();
                post([this]() {
                    refreshTable();
                    invalidate();
                }, 3000);
            }, 3000);
        }, 1500);
    }

private:
    App* App_ = nullptr;
    Table table_;
    TextInput gid_, gname_;
};

// --------------------------------------------------- 改名投票对话框 --
class RenameVoteForm : public Form {
public:
    RenameVoteForm(App* app, const nw::Json& info) : App_(app), info_(info) {}

protected:
    void buildLayout() override {
        const int pad = 22;
        int w = clientW_;
        std::wstring from = U8(info_.get("from_name").asStr("?"));
        std::wstring oldN = U8(info_.get("old").asStr(""));
        std::wstring newN = U8(info_.get("new").asStr(""));
        int need = (int)App_->core()->cfg.get("rename_vote_need").asInt(5);
        addLabel(L"「" + from + L"」申请改名", pad, 18, w - 2 * pad, textH(12, true), 12, true,
                 C_TEXT, 0, false);
        int y = 18 + textH(12, true) + 8;
        addLabel(L"从「" + oldN + L"」改为「" + newN + L"」", pad, y, w - 2 * pad, textH(11, false),
                 11, false, C_TEXT, 0, false);
        y += textH(11, false) + 2;
        y += 4;
        wchar_t buf[256];
        swprintf(buf, 256, L"按约定：首次改名自由，之后需超过 %d 名用户同意。\n你同意这次改名吗？",
                 need);
        addLabel(buf, pad, y, 320, textH(9, false) * 2 + 4, 9, false, C_SUB, 0, true);
        y += textH(9, false) * 2 + 4 + 14;
        int bh = textH(10, false) + 8;
        int bw = textW(L"0000000000", 10, false) + 6;
        addButton(2, L"拒绝", w - pad - bw, y, bw, bh, 10, false, false, true);
        addButton(1, L"同意", w - pad - bw - 8 - bw, y, bw, bh, 10, true, true, false);
    }
    void onClick(int id, int val) override {
        (void)val;
        std::string pid = info_.get("pid").asStr("");
        try {
            App_->core()->cast_rename_vote(pid, id == 1);
        } catch (...) {
        }
        close(id);
    }
    bool onCloseRequest() override { return false; }        // 必须投票，不能直接关掉
    void onCreated() override {
        int w = 400, h = 210;
        fitToClient(w, h);
        centerRightBottom(w, h);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

private:
    App* App_ = nullptr;
    nw::Json info_;
};

// --------------------------------------------------- 更改投票对话框 --
class ChangeVoteForm : public Form {
public:
    ChangeVoteForm(App* app, const Card& c) : App_(app), card_(c) {}

protected:
    void buildLayout() override {
        const int pad = 20;
        int w = clientW_;
        addLabel(L"「" + card_.name + L"」入网申请", pad, 16, w - 2 * pad, textH(11, true), 11,
                 true, C_TEXT, 0, false);
        std::wstring cur = card_.my_choice == 1 ? L"同意" : (card_.my_choice == 0 ? L"拒绝" : L"弃权");
        if (!card_.voted) cur = L"未投票";
        std::wstring info = L"物理地址 " + card_.mac_pretty + L" · 当前我的投票：" + cur +
                            L"（还剩 " + fmtRemainW(card_.change_left) + L" 可改）";
        int y = 16 + textH(11, true) + 6;
        addLabel(info, pad, y, w - 2 * pad, textH(9, false), 9, false, C_SUB, 0, true);
        y += textH(9, false) + 12;
        int bh = textH(10, true) + 8;
        int bw = textW(L"同意", 10, true) + 28;
        addButton(1, L"同意", pad, y, bw, bh, 10, true, true, false);
        addButton(2, L"拒绝", pad + bw + 8, y, bw, bh, 10, true, false, true);
        {
            FItem& it = addButton(3, L"弃权", pad + 2 * (bw + 8), y, bw, bh, 10, true, false, true);
            it.fg = C_SUB;
        }
        addButton(4, L"取消", pad + 3 * (bw + 8) + 8, y, textW(L"取消", 10, false) + 20, bh, 10,
                  false, false, false);
    }
    void onClick(int id, int val) override {
        (void)val;
        if (id == 4) {
            close(0);
            return;
        }
        int v = id == 1 ? 1 : (id == 2 ? 0 : -1);
        App_->castVote(card_.req_id, v);
        close(1);
    }
    void onCreated() override {
        int w = 460;
        int h = 16 + textH(11, true) + 6 + textH(9, false) + 12 + (textH(10, true) + 8) + 16;
        fitToClient(w, h);
        centerOnOwner(w, h, 3);
    }

private:
    App* App_ = nullptr;
    Card card_;
};

// --------------------------------------------------- 发现新版本 --
class UpdateForm : public Form {
public:
    UpdateForm(App* app, const nw::Json& res) : App_(app), res_(res) {}

protected:
    void buildLayout() override {
        const int pad = 20;
        int w = clientW_;
        std::wstring remote = U8(res_.get("remote").asStr("?"));
        std::wstring local = U8(res_.get("local").asStr(nw::APP_VERSION_FALLBACK));
        std::wstring notes = trimW(U8(res_.get("notes").asStr("")));
        std::wstring msg = L"新版本 —— v" + remote + L"\n\n";
        if (!notes.empty()) msg += L"更新内容：\n" + notes + L"\n\n";
        msg += L"（当前版本 v" + local + L"）\n\n";
        msg += L"「是」立即更新：启动同目录下的独立更新程序 neiwang_update1.exe，\n";
        msg += L"它会先把你的所有会话内容备份到 customer_god 文件夹，\n";
        msg += L"再取回新版本并把你的会话内容接着用；\n";
        msg += L"「否」打开更新页面手动下载；「取消」稍后再说。";
        std::vector<std::wstring> lines = wrapText(measureG(), msg, fontPt(10, false),
                                                   (float)(w - 2 * pad));
        int lh = textH(10, false);
        for (size_t i = 0; i < lines.size(); ++i) {
            addLabel(lines[i], pad, pad + (int)i * lh, w - 2 * pad, lh, 10, false, C_TEXT, 0,
                     false);
        }
        int by = pad + (int)lines.size() * lh + 16;
        int bh = textH(10, true) + 12;
        int bw = textW(L"取消", 10, true) + 40;
        addButton(3, L"取消", w - pad - bw, by, bw, bh, 10, false, false, true);
        addButton(2, L"否", w - pad - 2 * bw - 10, by, bw, bh, 10, true, false, true);
        addButton(1, L"是", w - pad - 3 * bw - 20, by, bw, bh, 10, true, true, false);
    }
    void onClick(int id, int val) override {
        (void)val;
        std::wstring page = U8(res_.get("page").asStr(""));
        if (page.empty()) page = U8(App_->core()->cfg.get("update_page").asStr(""));
        if (id == 1) {
            close(1);
            App_->startUpdate(page);
        } else if (id == 2) {
            close(2);
            if (!page.empty())
                ShellExecuteW(nullptr, L"open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            App_->setUpdateShown(false);      // 「取消」下次还能再提醒
            close(3);
        }
    }
    void onCreated() override {
        int w = 560;
        int h = 180;
        fitToClient(w, h);
        centerOnOwner(w, h, 3);
    }

private:
    App* App_ = nullptr;
    nw::Json res_;
};

// --------------------------------------------------- 空设备启动提示 --
class BlankForm : public Form {
public:
    explicit BlankForm(App* app) : App_(app) {}

protected:
    void buildLayout() override {
        const int pad = 20;
        int w = clientW_;
        addLabel(L"当前没有局域网网络活动，是否新建？", pad, 20, w - 2 * pad, textH(12, true), 12,
                 true, C_TEXT, 0, false);
        int y = 20 + textH(12, true) + 10;
        addLabel(L"未发现局域网内已有的内网与活动数据，你可以新建一个内网并创建第一个群组。", pad,
                 y, w - 2 * pad, textH(9, false) * 2, 9, false, C_SUB, 0, true);
        y += textH(9, false) * 2 + 16;
        int bh = textH(10, true) + 12;
        int b1 = textW(L"是，新建（推荐）", 10, true) + 32;
        int b2 = textW(L"否，稍后", 10, false) + 32;
        addButton(2, L"否，稍后", w - pad - b2, y, b2, bh, 10, false, false, true);
        addButton(1, L"是，新建（推荐）", w - pad - b2 - 10 - b1, y, b1, bh, 10, true, true, false);
    }
    void onClick(int id, int val) override {
        (void)val;
        if (id == 1) {
            nw::PeerCore* c = App_->core();
            // design_v2 3.2：先看附近有没有内网可以申请加入，没有才自建
            bool joined = false;
            try {
                std::vector<nw::Json> nearby = c->net_nearby_list();
                if (!nearby.empty()) {
                    std::string nid = nearby[0].get("nid").asStr("");
                    if (!nid.empty()) {
                        c->net_apply_join(nid);
                        joined = true;
                    }
                }
            } catch (...) {
                joined = false;
            }
            std::wstring name;
            if (joined) {
                name = U8(c->net.get("name").asStr(""));
            } else {
                try {
                    nw::Json n = c->net_create("");
                    name = U8(n.get("name").asStr(""));
                } catch (...) {
                }
            }
            // 与 Python 版一致：只写本机日志 + 记入活动历史（不广播，避免日志页出现重复行）
            std::wstring text = L"本机没有局域网活动数据，已按用户选择新建内网「" + name + L"」";
            App_->appendLog(nowStamp() + L" " + text);
            try {
                c->record_activity("sys", nw::ws2u8(text));
            } catch (...) {
            }
            close(1);
            App_->openCreateGroup();
        } else {
            std::wstring text = L"当前没有局域网网络活动（未新建）";
            App_->appendLog(nowStamp() + L" " + text);
            try {
                App_->core()->record_activity("sys", nw::ws2u8(text));
            } catch (...) {
            }
            close(2);
        }
    }
    void onCreated() override {
        int w = 460;
        int h = 20 + textH(12, true) + 10 + textH(9, false) * 2 + 16 + (textH(10, true) + 12) + 20;
        fitToClient(w, h);
        centerOnOwner(w, h, 3);
    }

private:
    App* App_ = nullptr;
};

// --------------------------------------------------- 群卡片右键菜单 --
class MenuForm : public Form {
public:
    MenuForm(App* app, const std::string& gid) : App_(app), gid_(gid) {}

protected:
    void buildLayout() override {
        int w = clientW_;
        std::wstring gname = U8(App_->core()->groups[gid_].get("gname").asStr(""));
        addLabel(L"群：" + gname + L"（" + U8(gid_) + L"）", 10, 4, w - 20, 24, 10, false, C_SUB,
                 0, true);
        FItem& ln = addLine(6, 30, w - 12);
        (void)ln;
        addButton(1, L"退出该群组", 2, 33, w - 4, 28, 10, false, false, false);
    }
    void onClick(int id, int val) override {
        (void)val;
        if (id == 1) {
            std::string gid = gid_;
            close(1);
            App_->leaveGroup(gid);
        } else {
            close(0);
        }
    }
    void onCreated() override {
        int w = textW(L"群：", 10, false) + 260;
        if (w > 420) w = 420;
        int h = 63;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
        fitToClient(w, h);
    }

private:
    App* App_ = nullptr;
    std::string gid_;
};

} // namespace

void openCreateGroupDialog(App* app) {
    if (!app) return;
    CreateGroupForm* f = new CreateGroupForm(app);
    if (!f->createForm(app->inst(), app->hwnd(), app, L"创建群组", 360, 210, false)) {
        delete f;
        return;
    }
    f->runModal();
    delete f;
}

void openJoinGroupDialog(App* app) {
    if (!app) return;
    JoinGroupForm* f = new JoinGroupForm(app);
    if (!f->createForm(app->inst(), app->hwnd(), app, L"加入群组", 560, 440, false)) {
        delete f;
        return;
    }
    f->runModal();
    delete f;
}

void showRenameVoteDialog(App* app, const nw::Json& info) {
    if (!app) return;
    RenameVoteForm* f = new RenameVoteForm(app, info);
    if (!f->createForm(app->inst(), app->hwnd(), app, L"改名投票", 400, 210, false)) {
        delete f;
        return;
    }
    f->runModal();
    delete f;
}

void openChangeVoteDialog(App* app, const Card& card) {
    if (!app) return;
    ChangeVoteForm* f = new ChangeVoteForm(app, card);
    if (!f->createForm(app->inst(), app->hwnd(), app, L"更改投票", 460, 160, false)) {
        delete f;
        return;
    }
    f->runModal();
    delete f;
}

void showUpdateDialog(App* app, const nw::Json& res) {
    if (!app) return;
    UpdateForm* f = new UpdateForm(app, res);
    if (!f->createForm(app->inst(), app->hwnd(), app, L"发现新版本", 560, 240, false)) {
        delete f;
        return;
    }
    f->runModal();
    delete f;
}

void showBlankDeviceDialog(App* app) {
    if (!app) return;
    BlankForm* f = new BlankForm(app);
    if (!f->createForm(app->inst(), app->hwnd(), app, U8(nw::APP_NAME), 460, 170, false)) {
        delete f;
        return;
    }
    f->runModal();
    delete f;
}

void showGroupMenu(App* app, const std::string& gid, int screenX, int screenY) {
    if (!app) return;
    if (app->core()->groups.find(gid) == app->core()->groups.end()) return;
    hideGroupMenu(app);
    MenuForm* f = new MenuForm(app, gid);
    if (!f->createForm(app->inst(), app->hwnd(), app, L"", 260, 63, false)) {
        delete f;
        return;
    }
    HWND h = f->hwnd();
    SetWindowPos(h, HWND_TOPMOST, screenX, screenY, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    f->runModal();                 // 模态：点击别处前必须先关掉菜单
    delete f;
}

void hideGroupMenu(App*) {}

// ------------------------------------------------------- 设置对话框 --
void openSettingsDialog(App* app) {
    if (!app) return;
    SettingsForm* f = new SettingsForm(app);
    if (!f->createForm(app->inst(), app->hwnd(), app, L"设置", 380, 350, false)) {
        delete f;
        return;
    }
    f->runModal();
    delete f;
}

void SettingsForm::onCreated() {
    int w = 380, h = 350;
    fitToClient(w, h);
    centerOnOwner(w, h, 3);
    name_.setText(U8(app_->core()->cfg.get("name").asStr("")));
    focus_ = &name_;
    name_.focused = true;
    app_->setSettingsDlg(this);
}

void SettingsForm::onDestroyed() { app_->onSettingsClosed(this); }

void SettingsForm::buildLayout() {
    const int pad = 22;
    int w = clientW_;
    addLabel(L"设置", pad, 18, w - 2 * pad, textH(14, true), 14, true, C_TEXT, 0, false);
    int y = 18 + textH(14, true) + 12;
    addLabel(L"我的名字（其他电脑看到的名字）", pad, y, w - 2 * pad, textH(9, false), 9, false,
             C_SUB, 0, false);
    y += textH(9, false) + 2;
    int ih = textH(11, false) + 12;
    addInput(&name_, pad, y, w - 2 * pad, ih, 11);
    y += ih + 4;
    bool needsVote = app_->core()->cfg.get("rename_count").asInt(0) > 0;
    int need = (int)app_->core()->cfg.get("rename_vote_need").asInt(5);
    std::wstring tip;
    if (needsVote) {
        wchar_t buf[256];
        swprintf(buf, 256, L"提示：已改过一次名字，再次改名需超过 %d 名用户投票同意后才能生效。",
                 need);
        tip = buf;
    } else {
        tip = L"提示：第一次改名可以直接保存；之后的修改需要同学们投票。";
    }
    {
        FItem& it = addLabel(tip, pad, y, w - 2 * pad, textH(8, false) * 2, 8, false, C_SUB, 0,
                             true);
        it.wrapW = 330;
    }
    y += textH(8, false) * 2 + 4;
    int by = y + 12;
    int bh = textH(10, true) + 10;
    int bw = textW(L"保存名字", 10, true) + 28;
    FItem& sb = addButton(1, L"保存名字", pad, by, bw, bh, 10, true, true, false);
    saveBtnId_ = sb.id;
    if (voting_) sb.disabled = true;
    addLabel(savedLbl_, pad + bw + 8, by, w - pad - bw - 8 - pad, bh, 9, false, C_ACCENT, 0, true);
    int ly = by + bh + 12;
    addLine(pad, ly, w - 2 * pad);
    ly += 12;
    addButton(2, L"检查更新（无网络 / 版本相同则静默，仅发现新版本才提醒）", pad, ly, w - 2 * pad,
              textH(10, false) + 6, 10, false, false, false);
    ly += textH(10, false) + 6 + 4;
    addButton(3, L"打开数据目录", pad, ly, w - 2 * pad, textH(10, false) + 6, 10, false, false,
              false);
    ly += textH(10, false) + 6 + 10;
    addLine(pad, ly, w - 2 * pad);
    ly += 10;
    wchar_t vb[256];
    swprintf(vb, 256, L"%s v%S · 局域网端口 UDP %d / TCP %d", U8(nw::APP_NAME).c_str(),
             nw::APP_VERSION_FALLBACK,
             (int)app_->core()->cfg.get("udp_port").asInt(0),
             (int)app_->core()->my_tcp_port());
    addLabel(vb, pad, ly, w - 2 * pad, textH(8, false), 8, false, C_SUB, 0, false);
}

void SettingsForm::onClick(int id, int val) {
    (void)val;
    if (id == 1) saveName();
    else if (id == 2) app_->onManualUpdate();
    else if (id == 3) app_->openDataDir();
}

void SettingsForm::onEnterKey() { saveName(); }

void SettingsForm::applyUi(const std::wstring& newName) {
    app_->core()->cfg.set("name", nw::Json(nw::ws2u8(newName)));
    app_->applyMyNameUi();
}

void SettingsForm::saveName() {
    if (voting_) return;
    std::wstring newName = trimW(name_.text);
    if (newName.empty()) {
        msgBox(app_, L"提示", L"名字不能为空", 0, hwnd_);
        return;
    }
    std::wstring cur = U8(app_->core()->cfg.get("name").asStr(""));
    if (newName == cur) {
        savedLbl_ = L"✓ 名字未改变";
        invalidate();
        post([this]() { savedLbl_.clear(); invalidate(); }, 1200);
        return;
    }
    bool needsVote = app_->core()->cfg.get("rename_count").asInt(0) > 0;
    int need = (int)app_->core()->cfg.get("rename_vote_need").asInt(5);
    if (!needsVote) {
        try {
            app_->core()->rename_self(nw::ws2u8(newName));
        } catch (...) {
        }
        applyUi(newName);
        savedLbl_ = L"✓ 已保存（第一次改名）";
        invalidate();
        post([this]() { savedLbl_.clear(); invalidate(); }, 1600);
        return;
    }
    voting_ = true;
    votingNew_ = newName;
    {
        wchar_t buf[256];
        swprintf(buf, 256, L"已发起投票：需超过 %d 名用户同意…", need);
        savedLbl_ = buf;
    }
    invalidate();
    try {
        app_->core()->start_rename_vote(nw::ws2u8(newName));
    } catch (const std::exception& e) {
        voting_ = false;
        votingNew_.clear();
        savedLbl_.clear();
        invalidate();
        msgBox(app_, L"发起投票失败", U8(e.what()), 2, hwnd_);
    } catch (...) {
        voting_ = false;
        votingNew_.clear();
        savedLbl_.clear();
        invalidate();
        msgBox(app_, L"发起投票失败", L"未知错误", 2, hwnd_);
    }
}

void SettingsForm::onVoteProgress(const std::string& pid, int approve, int reject) {
    (void)pid;
    (void)reject;
    if (!voting_) return;
    int need = (int)app_->core()->cfg.get("rename_vote_need").asInt(5);
    wchar_t buf[256];
    swprintf(buf, 256, L"投票中… 同意 %d 票（需超过 %d 票）", approve, need);
    savedLbl_ = buf;
    invalidate();
}

void SettingsForm::onVoteDone(const std::string& pid, bool ok, const std::wstring& reason,
                              const std::wstring& newName) {
    (void)pid;
    if (!voting_) return;
    voting_ = false;
    std::wstring target = votingNew_;
    votingNew_.clear();
    invalidate();
    if (ok) {
        applyUi(newName.empty() ? target : newName);
        savedLbl_ = L"✓ 投票通过，名字已更新";
        invalidate();
        post([this]() { savedLbl_.clear(); invalidate(); }, 2000);
    } else {
        savedLbl_.clear();
        invalidate();
        msgBox(app_, L"改名未通过", reason + L"\n\n可重新发起投票。", 1, hwnd_);
    }
}

// ------------------------------------------------------- 私聊窗口 --
ChatWindow::ChatWindow(App* app, const std::string& uid, const std::wstring& name) : uid_(uid),
                                                                                     name_(name) {
    (void)app;
}

ChatWindow::~ChatWindow() {}

std::wstring ChatWindow::titleText() const {
    std::wstring t = L"私聊 · " + name_;
    if (unread_ > 0) {
        wchar_t buf[64];
        swprintf(buf, 64, L"（%d 条新消息）", unread_);
        t += buf;
    }
    return t;
}

void ChatWindow::onCreated() {
    int w = 440, h = 580;
    fitToClient(w, h);
    centerLikeChat(w, h);
    relayout();
    if (hwnd_) SetWindowTextW(hwnd_, titleText().c_str());
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    focus_ = &entry_;
    entry_.focused = true;
    refreshStatus();
}

void ChatWindow::onDestroyed() {
    if (app_) {
        app_->unregisterDm(uid_);
        app_->reapForm(this);           // 由主窗口在退出时统一回收
    }
}

void ChatWindow::refreshStatus() {
    online_ = app_ && app_->core()->is_online(uid_);
    if (hwnd_) SetWindowTextW(hwnd_, titleText().c_str());
    invalidate();
}

void ChatWindow::markRead() {
    if (unread_) {
        unread_ = 0;
        if (hwnd_) SetWindowTextW(hwnd_, titleText().c_str());
    }
}

void ChatWindow::onActivated() { markRead(); }

bool ChatWindow::focused() const { return hwnd_ && GetForegroundWindow() == hwnd_; }

void ChatWindow::buildLayout() {
    int w = clientW_;
    int h = clientH_;
    rcHead_ = RECT{0, 0, w, 52};
    int inpH = 1 + 10 + (textH(10, false) + 14) + 10;
    rcInput_ = RECT{0, h - inpH, w, h};
    rcBody_ = RECT{0, 53, w, h - inpH};
    int sendW = textW(L"发送", 10, true) + 20;
    int sendH = textH(10, true) + 14;
    rcSend_ = RECT{w - 10 - sendW, rcInput_.top + 1 + 10, w - 10, rcInput_.top + 1 + 10 + sendH};
    rcEntry_ = RECT{10, rcSend_.top, rcSend_.left - 8, rcSend_.bottom};
    addInput(&entry_, rcEntry_.left, rcEntry_.top, rcEntry_.right - rcEntry_.left,
             rcEntry_.bottom - rcEntry_.top, 10);
    addButton(1, L"发送", rcSend_.left, rcSend_.top, rcSend_.right - rcSend_.left,
              rcSend_.bottom - rcSend_.top, 10, true, true, false);
}

void ChatWindow::onClick(int id, int val) {
    (void)val;
    if (id == 1) send();
}

void ChatWindow::onEnterKey() { send(); }

bool ChatWindow::onWheel(int x, int y, int notches) {
    if (!ptIn(rcBody_, x, y)) return false;
    int unit = (std::max)(20, (int)(rcBody_.bottom - rcBody_.top) / 10);
    sc_.offset += notches * unit;
    sc_.clamp();
    invalidate();
    return true;
}

void ChatWindow::send() {
    std::wstring text = trimW(entry_.text);
    if (text.empty()) return;
    if (!app_->core()->is_online(uid_)) {
        appendSys(L"对方不在线，正在寻找在线同学加密转存…");
    }
    app_->core()->send_dm_auto(uid_, nw::ws2u8(text));
    entry_.clear();
    invalidate();
}

void ChatWindow::appendSys(const std::wstring& text) {
    DmMsg m;
    m.sys = true;
    m.text = text;
    m.ts = nw::now_ts();
    msgs_.push_back(m);
    sc_.offset = 1 << 28;
    invalidate();
}

void ChatWindow::appendMsg(const nw::Json& j) {
    DmMsg m;
    m.sys = (j.get("t").asStr("") == "sys");
    if (m.sys) {
        m.text = U8(j.get("text").asStr(""));
    } else {
        m.self = j.get("self").asBool(false);
        m.from = U8(j.get("from_name").asStr("?"));
        m.text = U8(j.get("text").asStr(""));
        m.ts = j.get("ts").asNum(nw::now_ts());
        m.relayed = j.get("relayed").asBool(false);
    }
    msgs_.push_back(m);
    sc_.offset = 1 << 28;                 // 追加后立即滚到底
    invalidate();
}

void ChatWindow::onPaintExtra(Graphics& g, int w, int h) {
    // 头部
    fillRect(g, Gdiplus::RectF(0, 0, (REAL)w, 52), C_PANEL);
    drawAvatar(g, 12, 9, 34, name_);
    int tx = 12 + 34 + 8;
    drawText(g, name_, fontPt(11, true), C_TEXT,
             Gdiplus::RectF((REAL)tx, (REAL)7, (REAL)(w - tx - 8), (REAL)textH(11, true)), 0, 1,
             false, true);
    std::wstring st = online_ ? L"● 在线" : L"○ 可能离线";
    drawText(g, st, fontPt(8, false), online_ ? C_ACCENT : C_SUB,
             Gdiplus::RectF((REAL)tx, (REAL)(7 + textH(11, true)), (REAL)(w - tx - 8),
                            (REAL)textH(8, false)),
             0, 1, false, false);
    drawHDivider(g, 0, 52, w, C_BORDER);
    // 消息区
    fillRect(g, Gdiplus::RectF((REAL)rcBody_.left, (REAL)rcBody_.top,
                               (REAL)(rcBody_.right - rcBody_.left),
                               (REAL)(rcBody_.bottom - rcBody_.top)), C_BG);
    sc_.rc = rcBody_;
    // 行高
    std::vector<int> hs;
    hs.reserve(msgs_.size());
    for (auto& m : msgs_) {
        if (m.sys) {
            hs.push_back(textH(9, false) + 6);
            continue;
        }
        std::vector<std::wstring> lines = wrapText(measureG(), m.text, fontPt(10, false), 300.f);
        int bh = (int)lines.size() * textH(10, false) + 16;
        int rh = bh;
        if (!m.self) rh += 30 + 2;
        rh += 2 + textH(8, false);
        hs.push_back(rh + 8);
    }
    int contentH = 0;
    for (int x : hs) contentH += x;
    sc_.contentH = contentH;
    sc_.clamp();
    int y = sc_.rc.top - sc_.offset;
    for (size_t i = 0; i < msgs_.size(); ++i) {
        int top = y;
        y += hs[i];
        const DmMsg& m = msgs_[i];
        if (m.sys) {
            drawText(g, L"── " + m.text + L" ──", fontPt(9, false), C_SUB,
                     Gdiplus::RectF((REAL)sc_.rc.left, (REAL)(top + 3),
                                    (REAL)(sc_.rc.right - sc_.rc.left), (REAL)textH(9, false)),
                     1, 1, false, false);
            continue;
        }
        int x0 = sc_.rc.left + 14;
        int x1 = sc_.rc.right - 14;
        int by = top + 4;
        std::vector<std::wstring> lines = wrapText(measureG(), m.text, fontPt(10, false), 300.f);
        int bh = (int)lines.size() * textH(10, false) + 16;
        int bw = 0;
        for (auto& ln : lines) bw = (std::max)(bw, g_textW(ln, 10, false));
        if (m.self) {
            int brx = x1 - (bw + 24);
            Gdiplus::RectF bub((REAL)brx, (REAL)by, (REAL)(bw + 24), (REAL)bh);
            fillRound(g, bub, 4.f, C_BUBBLE_SELF);
            int ly = by + 8;
            for (auto& ln : lines) {
                drawText(g, ln, fontPt(10, false), C_TEXT,
                         Gdiplus::RectF((REAL)(brx + 12), (REAL)ly, (REAL)(bw + 4),
                                        (REAL)textH(10, false)),
                         0, 1, false, false);
                ly += textH(10, false);
            }
            drawText(g, timeHMS(m.ts) + L"  我", fontPt(8, false), C_SUB,
                     Gdiplus::RectF((REAL)x0, (REAL)(by + bh + 2), (REAL)(x1 - x0),
                                    (REAL)textH(8, false)),
                     2, 1, false, false);
        } else {
            drawAvatar(g, (REAL)x0, (REAL)by, 30, m.from);
            std::wstring info = m.from + L"  " + timeHMS(m.ts);
            if (m.relayed) info += L"  · 离线转存送达";
            drawText(g, info, fontPt(8, false), C_SUB,
                     Gdiplus::RectF((REAL)(x0 + 36), (REAL)by, (REAL)(x1 - x0 - 36),
                                    (REAL)textH(8, false)),
                     0, 1, false, true);
            int byy = by + 30 + 2;
            Gdiplus::RectF bub((REAL)x0, (REAL)byy, (REAL)(bw + 24), (REAL)bh);
            fillRound(g, bub, 4.f, C_BUBBLE_OTHER);
            strokeRound(g, bub, 4.f, C_BORDER, 1.f);
            int ly = byy + 8;
            for (auto& ln : lines) {
                drawText(g, ln, fontPt(10, false), C_TEXT,
                         Gdiplus::RectF((REAL)(x0 + 12), (REAL)ly, (REAL)(bw + 4),
                                        (REAL)textH(10, false)),
                         0, 1, false, false);
                ly += textH(10, false);
            }
        }
    }
    if (sc_.contentH > sc_.viewH()) {
        RECT track{sc_.rc.right - SCROLLBAR_W, sc_.rc.top, sc_.rc.right, sc_.rc.bottom};
        drawScrollBar(g, track, sc_.contentH, sc_.viewH(), sc_.offset, false);
    }
    // 输入区
    fillRect(g, Gdiplus::RectF((REAL)rcInput_.left, (REAL)rcInput_.top,
                               (REAL)(rcInput_.right - rcInput_.left),
                               (REAL)(rcInput_.bottom - rcInput_.top)), C_PANEL);
    drawHDivider(g, rcInput_.left, rcInput_.top, rcInput_.right, C_BORDER);
}

ChatWindow* openChatWindow(App* app, const std::string& uid, const std::wstring& name) {
    if (!app) return nullptr;
    ChatWindow* w = app->findDm(uid);
    if (w) return w;
    ChatWindow* cw = new ChatWindow(app, uid, name);
    if (!cw->createForm(app->inst(), app->hwnd(), app, L"私聊 · " + name, 440, 580, false)) {
        delete cw;
        return nullptr;
    }
    app->registerDm(uid, cw);
    return cw;
}

} // namespace ui
