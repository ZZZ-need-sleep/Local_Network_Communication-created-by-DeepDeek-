// ui_common.h —— Win32 + GDI+ 界面公共层：调色板、字体缓存、绘制/排版助手、
//                单行自绘文本框、滚动区域与热点模型。
// 纯自绘界面，不使用任何标准控件；所有尺寸单位 = 设备像素。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <objbase.h>
#include <gdiplus.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")

namespace ui {

// ------------------------------------------------------------------ 颜色 --
// 与 neiwang.py 的 class P 逐字对应（sRGB）。
inline Gdiplus::Color RGBc(unsigned hex, BYTE a = 255) {
    return Gdiplus::Color(a, (BYTE)((hex >> 16) & 0xFF), (BYTE)((hex >> 8) & 0xFF),
                          (BYTE)(hex & 0xFF));
}

inline const Gdiplus::Color C_BG          = RGBc(0xf7f7f7);
inline const Gdiplus::Color C_PANEL       = RGBc(0xffffff);
inline const Gdiplus::Color C_HOVER       = RGBc(0xf2f2f2);
inline const Gdiplus::Color C_SELECTED    = RGBc(0xe8f7ee);
inline const Gdiplus::Color C_ACCENT      = RGBc(0x07c160);
inline const Gdiplus::Color C_ACCENT_HV   = RGBc(0x06ad56);
inline const Gdiplus::Color C_TEXT        = RGBc(0x191919);
inline const Gdiplus::Color C_SUB         = RGBc(0x9a9a9a);
inline const Gdiplus::Color C_BORDER      = RGBc(0xe6e6e6);
inline const Gdiplus::Color C_BUBBLE_SELF = RGBc(0x95ec69);
inline const Gdiplus::Color C_BUBBLE_OTHER= RGBc(0xffffff);
inline const Gdiplus::Color C_BADGE       = RGBc(0xf76260);
inline const Gdiplus::Color C_LOG_BG      = RGBc(0xf5f8fc);
inline const Gdiplus::Color C_ENTRY_BG    = RGBc(0xf3f3f3);
inline const Gdiplus::Color C_WHITE       = RGBc(0xffffff);
inline const Gdiplus::Color C_SCROLL_THUMB= RGBc(0xc8c8c8);
inline const Gdiplus::Color C_SCROLL_TROUGH = RGBc(0xf0f0f0);
inline const Gdiplus::Color C_TOOLTIP_BG  = RGBc(0xffffff);

// 头像配色（AVATAR_COLORS，顺序敏感）
extern const unsigned AVATAR_COLORS[10];
Gdiplus::Color avatarColor(const std::wstring& name);

// ------------------------------------------------------------------ 字体 --
// pt 为「点」，与 Python/Tk 的字号一一对应（FAMILY 10 → 10 点）。
Gdiplus::Font* fontPt(int pt, bool bold = false);
Gdiplus::Font* emojiFontPt(int pt);
Gdiplus::Font* monoFontPt(int pt);
Gdiplus::Graphics& measureG();       // 供排版测量用（离屏）

// ------------------------------------------------------------------ 文本 --
std::wstring U8(const std::string& utf8);
std::string  W8(const std::wstring& w);
std::wstring trimW(const std::wstring& s);
std::wstring lowerW(const std::wstring& s);
std::wstring leftChars(const std::wstring& s, size_t n);   // 等价 Python s[:n]

Gdiplus::SizeF measureText(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f,
                           float maxW = 100000.0f);
std::vector<std::wstring> wrapText(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f,
                                   float maxW);

// align: 0 左 1 中 2 右   valign: 0 上 1 中 2 下
void drawText(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f,
              const Gdiplus::Color& col, const Gdiplus::RectF& rc,
              int align = 0, int valign = 1, bool wrap = false, bool ellipsis = false);

// ------------------------------------------------------------------ 形状 --
void fillRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, const Gdiplus::Color& c);
void fillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float rad, const Gdiplus::Color& c);
void strokeRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float rad,
                 const Gdiplus::Color& c, float w = 1.0f);
void fillCircle(Gdiplus::Graphics& g, const Gdiplus::RectF& r, const Gdiplus::Color& c);
void drawAvatar(Gdiplus::Graphics& g, float x, float y, int size, const std::wstring& name);
void drawHDivider(Gdiplus::Graphics& g, int x1, int y, int x2, const Gdiplus::Color& c);
void drawVDivider(Gdiplus::Graphics& g, int x, int y1, int y2, const Gdiplus::Color& c);
// 统一样式的自绘按钮（primary=绿色实心；outline=1px 实线边；否则扁平）
void drawStdButton(Gdiplus::Graphics& g, const RECT& rc, const std::wstring& text, int pt,
                   bool bold, bool primary, bool outline, bool hover, bool disabled,
                   const Gdiplus::Color& bg, const Gdiplus::Color& fg);

// ------------------------------------------------------------- 滚动条 --
const int SCROLLBAR_W = 8;                      // ScrollFrame 的 8px 滚动条
void drawScrollBar(Gdiplus::Graphics& g, const RECT& track, int contentH, int viewH, int offset,
                   bool hovering);
int  scrollThumbTop(const RECT& track, int contentH, int viewH, int offset);
int  scrollThumbH(const RECT& track, int contentH, int viewH);

// --------------------------------------------------------------- 时间 --
std::wstring timeHMS(double ts);        // "HH:MM:SS"      （fmt_time(ts)[5:]）
std::wstring timeFull(double ts);       // "MM-DD HH:MM:SS"
std::wstring nowStamp();                // "[MM-DD HH:MM:SS]"
std::wstring fmtMacW(const std::string& uid);
std::wstring humanSizeW(long long n);
std::wstring fmtRemainW(double sec);    // _fmt_remain 的 C++ 版（逐字复刻）

// ------------------------------------------------------- 单行自绘文本框 --
struct TextInput {
    std::wstring text;
    size_t caret = 0;
    int    scrollX = 0;
    bool   focused = false;
    bool   placeholder = false;            // 处于占位态
    std::wstring phText;
    double caretBlink = 0.0;

    void setText(const std::wstring& t) { text = t; caret = t.size(); scrollX = 0; }
    std::wstring value() const { return placeholder ? std::wstring() : text; }
    bool empty() const { return placeholder || text.empty(); }
    void clear() { text.clear(); caret = 0; scrollX = 0; }
    void setPlaceholder(const std::wstring& ph) { placeholder = true; text = ph; phText = ph; caret = text.size(); }
    void clearPlaceholder() { placeholder = false; text.clear(); caret = 0; scrollX = 0; }

    void insertChar(wchar_t ch);
    void backspace();
    void del();
    void moveLeft();
    void moveRight();
    void home();
    void end();
    int  caretX(Gdiplus::Graphics& g, Gdiplus::Font* f, int textLeft) const;
    int  hitIndex(Gdiplus::Graphics& g, Gdiplus::Font* f, int textLeft, int x) const;
};

// --------------------------------------------------------------- 热点 --
// 每次排版时重建；点击时重新排版再命中，避免使用过期的矩形。
struct Hot {
    RECT rc{};
    int  act = 0;
    std::string arg;      // 主参数（gid / uid / req_id / fid ...）
    int  val = 0;
    bool enabled = true;
    bool hoverable = false;
};

struct ScrollArea {
    RECT rc{};
    int  contentH = 0;
    int  offset = 0;
    bool hover = false;              // 指针是否在区域内（决定滚轮是否生效）
    int  maxOffset() const {
        int vh = (int)(rc.bottom - rc.top);
        return contentH - vh > 0 ? contentH - vh : 0;
    }
    int  viewH() const { return (int)(rc.bottom - rc.top); }
    void clamp() { offset = offset < 0 ? 0 : (offset > maxOffset() ? maxOffset() : offset); }
    bool contains(int x, int y) const { return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom; }
};

inline bool ptIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// --------------------------------------------------------------- 动作 ID --
// 主窗口与对话框共享的动作标识（自绘热点的 act 字段）。
enum Act {
    A_NONE = 0,
    A_NAV_CHAT, A_NAV_NET, A_NAV_VOTE, A_NAV_MORE, A_NAV_LOG,
    A_MY_AVATAR, A_SETTINGS_BTN,
    A_CREATE_GROUP, A_JOIN_GROUP,
    A_SEARCH_FOCUS,
    A_GROUP_CARD,                 // arg=gid
    A_MENU_LEAVE,                 // arg=gid
    A_MENU_CANCEL,
    A_CHIP_CHAT, A_CHIP_FILES, A_CHIP_MEMBERS,
    A_LEAVE_GROUP,
    A_CHAT_ENTRY, A_SEND_CHAT, A_SEND_FILE,
    A_MEMBER_CARD,                // arg=uid
    A_FILE_ROW,                   // arg=fid
    A_FILE_OPEN, A_FILE_RETRY, A_FILE_OPENDIR,
    A_NET_SCAN, A_NET_CREATE, A_NET_RENAME, A_NET_LEAVE, A_NET_APPLY,
    A_NEARBY_ROW,                 // arg=nid
    A_NETMEMBER_ROW,
    A_VOTE_CAST,                  // arg=req_id, val: 1 同意 / 0 拒绝 / -1 弃权
    A_VOTE_CANCEL_APPLY,
    A_VOTE_PAGE_OPEN,
    A_CHANGE_VOTE,
    A_HIST_ROW,                   // arg=req_id
    A_TOOL_DATADIR, A_TOOL_UPDATE, A_TOOL_SETTINGS,
    A_CLEAR_LOG,
    A_NOTIFY_CLOSE,
    A_DLG_OK, A_DLG_CANCEL, A_DLG_EXTRA1, A_DLG_EXTRA2, A_DLG_EXTRA3,
    A_DLG_ROW                     // 对话框表格行
};

// --------------------------------------------------------------- 表格 --
const int TABLE_SCROLL_W = 14;    // ttk 默认滚动条宽度的近似值

struct Table {
    std::vector<std::wstring> headers;
    std::vector<float> widths;                 // 基础列宽（按可用宽度等比拉伸）
    std::vector<std::vector<std::wstring>> rows;
    std::vector<std::string> iids;
    int  rowH = 30;
    int  sel = -1;
    int  offset = 0;
    int  heightRows = 8;                       // ttk height=
    RECT rc{};                                 // 整体（含表头）
    bool withScrollbar = true;

    RECT headerRect() const { RECT r{rc.left, rc.top, rc.right, rc.top + rowH}; return r; }
    RECT bodyRect() const { RECT r{rc.left, rc.top + rowH, rc.right, rc.bottom}; return r; }
    RECT rowsRect() const {
        RECT r = bodyRect();
        if (withScrollbar) r.right -= TABLE_SCROLL_W;
        return r;
    }
    int  contentH() const { return (int)rows.size() * rowH; }
    int  maxOffset() const {
        int vh = (int)(rowsRect().bottom - rowsRect().top);
        return contentH() - vh > 0 ? contentH() - vh : 0;
    }
    void clamp() { offset = offset < 0 ? 0 : (offset > maxOffset() ? maxOffset() : offset); }
    int  rowAt(int x, int y) const;
};

void drawTable(Gdiplus::Graphics& g, const Table& t);

// --------------------------------------------------------------- 投票卡 --
struct Card {
    std::string req_id, state, note, mac, ip;
    std::wstring name, mac_pretty, desc;
    int  approvals = 0, rejects = 0, abstains = 0, voters = 0, need = 0;
    bool from_me = false, voted = false, can_change = false;
    int  my_choice = 99;                 // 1 同意 / 0 拒绝 / -1 弃权 / 99 未投票
    double remain = 0, change_left = 0;
};

Card cardFromJson(const nw::Json& j);
bool cardCanVote(const Card& c, nw::PeerCore* core);
int  cardHeight(Gdiplus::Graphics& g, const Card& c, int width, nw::PeerCore* core);
// 在 rc 内绘制一张投票卡片；命中区域追加到 hits。返回实际占用高度。
void drawVoteCard(Gdiplus::Graphics& g, const RECT& rc, const Card& c, std::vector<Hot>& hits,
                  nw::PeerCore* core);

// GDI+ 色 → Win32 刷子色（仅用于背景擦除）
inline COLORREF toCOLORREF(const Gdiplus::Color& c) {
    return RGB(c.GetR(), c.GetG(), c.GetB());
}

// 双缓冲绘制上下文
struct DblBuffer {
    HDC     hdc = nullptr;
    HDC     mem = nullptr;
    HBITMAP bmp = nullptr;
    HBITMAP old = nullptr;
    int     w = 0, h = 0;
    bool begin(HDC target, int width, int height);
    void end();
    void release();
};

} // namespace ui
