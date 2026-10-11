// ui_common.cpp —— 公共绘制/排版实现
#include "ui_common.h"

#include "../common/util.h"

#include <cmath>
#include <cstdio>
#include <cwctype>

namespace ui {

const unsigned AVATAR_COLORS[10] = {0x07c160, 0x576b95, 0xf76260, 0x10aeff, 0xffc300,
                                    0x8a2be2, 0xff9f40, 0x00b0b9, 0xe062a4, 0x5f9ea0};

// ------------------------------------------------------------------ 字体 --
static std::map<int, Gdiplus::Font*>* g_fonts = nullptr;
static std::map<int, Gdiplus::Font*>* g_emoji = nullptr;
static std::map<int, Gdiplus::Font*>* g_mono = nullptr;

Gdiplus::Font* fontPt(int pt, bool bold) {
    if (!g_fonts) g_fonts = new std::map<int, Gdiplus::Font*>();
    int key = pt * 4 + (bold ? 1 : 0);
    auto it = g_fonts->find(key);
    if (it != g_fonts->end()) return it->second;
    Gdiplus::Font* f = new Gdiplus::Font(L"Microsoft YaHei UI", (Gdiplus::REAL)pt,
                                         bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                                         Gdiplus::UnitPoint);
    if (f->GetLastStatus() != Gdiplus::Ok) {
        delete f;
        f = new Gdiplus::Font(L"Microsoft YaHei", (Gdiplus::REAL)pt,
                              bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                              Gdiplus::UnitPoint);
        if (f->GetLastStatus() != Gdiplus::Ok) {
            delete f;
            f = new Gdiplus::Font(L"Arial", (Gdiplus::REAL)pt,
                                  bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                                  Gdiplus::UnitPoint);
        }
    }
    (*g_fonts)[key] = f;
    return f;
}

Gdiplus::Font* emojiFontPt(int pt) {
    if (!g_emoji) g_emoji = new std::map<int, Gdiplus::Font*>();
    auto it = g_emoji->find(pt);
    if (it != g_emoji->end()) return it->second;
    Gdiplus::Font* f = new Gdiplus::Font(L"Segoe UI Emoji", (Gdiplus::REAL)pt,
                                         Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    if (f->GetLastStatus() != Gdiplus::Ok) {
        delete f;
        f = fontPt(pt, false);
    }
    (*g_emoji)[pt] = f;
    return f;
}

Gdiplus::Font* monoFontPt(int pt) {
    if (!g_mono) g_mono = new std::map<int, Gdiplus::Font*>();
    auto it = g_mono->find(pt);
    if (it != g_mono->end()) return it->second;
    Gdiplus::Font* f = new Gdiplus::Font(L"Consolas", (Gdiplus::REAL)pt,
                                         Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    if (f->GetLastStatus() != Gdiplus::Ok) {
        delete f;
        f = fontPt(pt, false);
    }
    (*g_mono)[pt] = f;
    return f;
}

Gdiplus::Graphics& measureG() {
    static HDC sdc = nullptr;
    static Gdiplus::Graphics* g = nullptr;
    if (!g) {
        sdc = GetDC(nullptr);
        g = new Gdiplus::Graphics(sdc);
        g->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    }
    return *g;
}

Gdiplus::Color avatarColor(const std::wstring& nameIn) {
    std::wstring name = nameIn.empty() ? L"?" : nameIn;
    unsigned sum = 0;
    for (wchar_t c : name) sum += (unsigned)c;
    return RGBc(AVATAR_COLORS[sum % 10]);
}

// ------------------------------------------------------------------ 文本 --
std::wstring U8(const std::string& utf8) { return nw::u82ws(utf8); }
std::string  W8(const std::wstring& w) { return nw::ws2u8(w); }

std::wstring trimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

std::wstring lowerW(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = (wchar_t)towlower(c);
    return r;
}

std::wstring leftChars(const std::wstring& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n);
}

static Gdiplus::StringFormat& measureFormat() {
    static Gdiplus::StringFormat* sf = nullptr;
    if (!sf) {
        sf = new Gdiplus::StringFormat(Gdiplus::StringFormat::GenericTypographic());
        sf->SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap |
                           Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    }
    return *sf;
}

Gdiplus::SizeF measureText(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f,
                           float maxW) {
    Gdiplus::SizeF out(0.f, 0.f);
    if (s.empty()) return out;
    Gdiplus::RectF layout(0.f, 0.f, maxW, 100000.f);
    Gdiplus::RectF bounds;
    g.MeasureString(s.c_str(), (INT)s.size(), f, layout, &measureFormat(), &bounds);
    out.Width = bounds.Width;
    out.Height = bounds.Height;
    return out;
}

std::vector<std::wstring> wrapText(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f,
                                   float maxW) {
    std::vector<std::wstring> lines;
    if (s.empty()) { lines.push_back(L""); return lines; }
    // 按显式换行切段
    std::vector<std::wstring> paras;
    {
        std::wstring cur;
        for (wchar_t c : s) {
            if (c == L'\n') { paras.push_back(cur); cur.clear(); }
            else if (c == L'\r') continue;
            else cur.push_back(c);
        }
        paras.push_back(cur);
    }
    for (auto& para : paras) {
        if (para.empty()) { lines.push_back(L""); continue; }
        // 切成不可分割单元：连续 ASCII 字母数字下划线算一个词，其余单字符一词，
        // 词尾空格并入该词（与 Tk wraplength 的近似行为一致）。
        std::vector<std::wstring> units;
        size_t i = 0;
        while (i < para.size()) {
            std::wstring u;
            wchar_t c = para[i];
            bool wordy = (c < 128) && (iswalnum(c) || c == L'_');
            if (wordy) {
                while (i < para.size() && (para[i] < 128) && (iswalnum(para[i]) || para[i] == L'_'))
                    u.push_back(para[i++]);
                while (i < para.size() && para[i] == L' ') u.push_back(para[i++]);
            } else if (c == L' ') {
                while (i < para.size() && para[i] == L' ') u.push_back(para[i++]);
            } else {
                u.push_back(para[i++]);
            }
            units.push_back(u);
        }
        std::wstring cur;
        float curW = 0.f;
        for (auto& u : units) {
            float uw = measureText(g, u, f).Width;
            float spW = 0.f;
            if (!cur.empty()) spW = measureText(g, L" ", f).Width;
            if (!cur.empty() && curW + spW + uw > maxW) {
                lines.push_back(cur);
                cur = u;
                curW = uw;
            } else {
                if (!cur.empty() && spW > 0 && u != L" ") { cur += L" "; curW += spW; }
                cur += u;
                curW += uw;
            }
        }
        lines.push_back(cur);
    }
    if (lines.empty()) lines.push_back(L"");
    return lines;
}

void drawText(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f,
              const Gdiplus::Color& col, const Gdiplus::RectF& rc,
              int align, int valign, bool wrap, bool ellipsis) {
    if (s.empty()) return;
    Gdiplus::StringFormat sf;
    sf.SetAlignment(align == 1 ? Gdiplus::StringAlignmentCenter
                               : (align == 2 ? Gdiplus::StringAlignmentFar
                                             : Gdiplus::StringAlignmentNear));
    sf.SetLineAlignment(valign == 0 ? Gdiplus::StringAlignmentNear
                                    : (valign == 2 ? Gdiplus::StringAlignmentFar
                                                   : Gdiplus::StringAlignmentCenter));
    UINT flags = 0;
    if (!wrap) flags |= Gdiplus::StringFormatFlagsNoWrap;
    if (ellipsis) {
        sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        flags |= Gdiplus::StringFormatFlagsNoClip;
    }
    sf.SetFormatFlags(flags);
    Gdiplus::SolidBrush br(col);
    g.DrawString(s.c_str(), (INT)s.size(), f, rc, &sf, &br);
}

// ------------------------------------------------------------------ 形状 --
void fillRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, const Gdiplus::Color& c) {
    Gdiplus::SolidBrush b(c);
    g.FillRectangle(&b, r);
}

void fillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float rad, const Gdiplus::Color& c) {
    Gdiplus::GraphicsPath p;
    float d = rad * 2.f;
    if (rad <= 0.f) { fillRect(g, r, c); return; }
    p.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
    p.CloseFigure();
    Gdiplus::SolidBrush b(c);
    g.FillPath(&b, &p);
}

void strokeRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float rad,
                 const Gdiplus::Color& c, float w) {
    Gdiplus::GraphicsPath p;
    float d = rad * 2.f;
    if (rad <= 0.f) {
        Gdiplus::Pen pen(c, w);
        g.DrawRectangle(&pen, r);
        return;
    }
    p.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
    p.CloseFigure();
    Gdiplus::Pen pen(c, w);
    g.DrawPath(&pen, &p);
}

void fillCircle(Gdiplus::Graphics& g, const Gdiplus::RectF& r, const Gdiplus::Color& c) {
    Gdiplus::SolidBrush b(c);
    g.FillEllipse(&b, r);
}

void drawAvatar(Gdiplus::Graphics& g, float x, float y, int size, const std::wstring& nameIn) {
    std::wstring name = nameIn.empty() ? L"?" : nameIn;
    Gdiplus::Color col = avatarColor(name);
    Gdiplus::RectF rc(x + 1.f, y + 1.f, (float)(size - 2), (float)(size - 2));
    fillCircle(g, rc, col);
    int pt = (std::max)(9, (int)(size * 0.38));
    Gdiplus::Font* f = fontPt(pt, true);
    std::wstring ch = leftChars(name, 1);
    Gdiplus::RectF tr(x, y, (float)size, (float)size);
    drawText(g, ch, f, C_WHITE, tr, 1, 1, false, false);
}

void drawHDivider(Gdiplus::Graphics& g, int x1, int y, int x2, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.f);
    g.DrawLine(&pen, (Gdiplus::REAL)x1, (Gdiplus::REAL)y + 0.5f, (Gdiplus::REAL)x2,
               (Gdiplus::REAL)y + 0.5f);
}

void drawVDivider(Gdiplus::Graphics& g, int x, int y1, int y2, const Gdiplus::Color& c) {
    Gdiplus::Pen pen(c, 1.f);
    g.DrawLine(&pen, (Gdiplus::REAL)x + 0.5f, (Gdiplus::REAL)y1, (Gdiplus::REAL)x + 0.5f,
               (Gdiplus::REAL)y2);
}

void drawStdButton(Gdiplus::Graphics& g, const RECT& rc, const std::wstring& text, int pt,
                   bool bold, bool primary, bool outline, bool hover, bool disabled,
                   const Gdiplus::Color& bgIn, const Gdiplus::Color& fgIn) {
    Gdiplus::RectF r((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                     (Gdiplus::REAL)(rc.right - rc.left), (Gdiplus::REAL)(rc.bottom - rc.top));
    Gdiplus::Color fg = fgIn;
    if (primary) {
        Gdiplus::Color bg = disabled ? RGBc(0xa8dfc0) : (hover ? RGBc(0x06ad56) : RGBc(0x07c160));
        fillRect(g, r, bg);
        fg = C_WHITE;
    } else {
        fillRect(g, r, hover && !disabled ? RGBc(0xf2f2f2) : bgIn);
        if (outline) {
            Gdiplus::Pen pen(disabled ? RGBc(0xeeeeee) : RGBc(0xe6e6e6), 1.f);
            g.DrawRectangle(&pen, (Gdiplus::REAL)rc.left + 0.5f, (Gdiplus::REAL)rc.top + 0.5f,
                            (Gdiplus::REAL)(rc.right - rc.left) - 1.f,
                            (Gdiplus::REAL)(rc.bottom - rc.top) - 1.f);
        }
    }
    drawText(g, text, fontPt(pt, bold), disabled ? RGBc(0x9a9a9a) : fg, r, 1, 1, false, false);
}

// ------------------------------------------------------------- 滚动条 --
int scrollThumbH(const RECT& track, int contentH, int viewH) {
    if (contentH <= viewH || contentH <= 0) return viewH;
    int h = (int)((double)viewH * (double)viewH / (double)contentH);
    return (std::max)(24, h);
}

int scrollThumbTop(const RECT& track, int contentH, int viewH, int offset) {
    int th = scrollThumbH(track, contentH, viewH);
    if (contentH <= viewH || contentH <= 0) return track.top;
    int span = (track.bottom - track.top) - th;
    int maxOff = contentH - viewH;
    if (maxOff <= 0) return track.top;
    return track.top + (int)((double)span * (double)offset / (double)maxOff);
}

void drawScrollBar(Gdiplus::Graphics& g, const RECT& track, int contentH, int viewH, int offset,
                   bool hovering) {
    Gdiplus::RectF tr((Gdiplus::REAL)track.left, (Gdiplus::REAL)track.top,
                      (Gdiplus::REAL)(track.right - track.left),
                      (Gdiplus::REAL)(track.bottom - track.top));
    fillRect(g, tr, C_SCROLL_TROUGH);
    if (contentH <= viewH || contentH <= 0) {
        fillRect(g, tr, C_SCROLL_TROUGH);
        return;
    }
    int th = scrollThumbH(track, contentH, viewH);
    int tt = scrollThumbTop(track, contentH, viewH, offset);
    Gdiplus::RectF thumb((Gdiplus::REAL)track.left, (Gdiplus::REAL)tt,
                         (Gdiplus::REAL)(track.right - track.left), (Gdiplus::REAL)th);
    fillRect(g, thumb, hovering ? RGBc(0xb4b4b4) : C_SCROLL_THUMB);
}

// --------------------------------------------------------------- 时间 --
std::wstring timeHMS(double ts) {
    std::string s = nw::fmt_time(ts);
    if (s.size() > 5) s = s.substr(5);
    return U8(s);
}

std::wstring timeFull(double ts) { return U8(nw::fmt_time(ts)); }

std::wstring nowStamp() {
    std::string s = nw::fmt_time(nw::now_ts());
    return U8("[" + s + "]");
}

std::wstring fmtMacW(const std::string& uid) { return U8(nw::fmt_mac(uid)); }

std::wstring humanSizeW(long long n) {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)n;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t buf[64];
    if (i == 0) swprintf(buf, 64, L"%lld B", n);
    else swprintf(buf, 64, L"%.1f %s", v, units[i]);
    return std::wstring(buf);
}

std::wstring fmtRemainW(double secIn) {
    long long sec = (long long)(std::max)(0.0, secIn);
    wchar_t buf[96];
    if (sec >= 86400) swprintf(buf, 96, L"%lld 天 %lld 小时", sec / 86400, (sec % 86400) / 3600);
    else if (sec >= 3600) swprintf(buf, 96, L"%lld 小时 %lld 分", sec / 3600, (sec % 3600) / 60);
    else if (sec >= 60) swprintf(buf, 96, L"%lld 分 %lld 秒", sec / 60, sec % 60);
    else swprintf(buf, 96, L"%lld 秒", sec);
    return std::wstring(buf);
}

// ------------------------------------------------------- 单行自绘文本框 --
void TextInput::insertChar(wchar_t ch) {
    if (ch < 0x20 && ch != L'\t') return;
    if (caret > text.size()) caret = text.size();
    text.insert(text.begin() + (ptrdiff_t)caret, ch);
    ++caret;
}

void TextInput::backspace() {
    if (caret == 0 || text.empty()) return;
    if (caret > text.size()) caret = text.size();
    text.erase(text.begin() + (ptrdiff_t)(caret - 1));
    --caret;
}

void TextInput::del() {
    if (caret >= text.size()) return;
    text.erase(text.begin() + (ptrdiff_t)caret);
}

void TextInput::moveLeft() { if (caret > 0) --caret; }
void TextInput::moveRight() { if (caret < text.size()) ++caret; }
void TextInput::home() { caret = 0; }
void TextInput::end() { caret = text.size(); }

int TextInput::caretX(Gdiplus::Graphics& g, Gdiplus::Font* f, int textLeft) const {
    size_t c = (std::min)(caret, text.size());
    std::wstring pre = text.substr(0, c);
    float w = pre.empty() ? 0.f : measureText(g, pre, f).Width;
    return textLeft + (int)w - scrollX;
}

int TextInput::hitIndex(Gdiplus::Graphics& g, Gdiplus::Font* f, int textLeft, int x) const {
    int target = x - textLeft + scrollX;
    if (target <= 0) return 0;
    float acc = 0.f;
    for (size_t i = 0; i < text.size(); ++i) {
        float cw = measureText(g, text.substr(i, 1), f).Width;
        if (acc + cw / 2.f >= (float)target) return (int)i;
        acc += cw;
    }
    return (int)text.size();
}

// --------------------------------------------------------------- 表格 --
int Table::rowAt(int x, int y) const {
    RECT rr = rowsRect();
    if (x < rr.left || x >= rr.right || y < rr.top || y >= rr.bottom) return -1;
    int idx = (y - rr.top + offset) / (rowH > 0 ? rowH : 1);
    if (idx < 0 || idx >= (int)rows.size()) return -1;
    return idx;
}

static std::vector<int> tableColumns(const Table& t) {
    RECT rr = t.rowsRect();
    int avail = (int)(rr.right - rr.left);
    std::vector<int> cols;
    float total = 0.f;
    for (float w : t.widths) total += w;
    if (total <= 0.f || t.widths.empty()) {
        cols.push_back(avail);
        return cols;
    }
    int used = 0;
    for (size_t i = 0; i < t.widths.size(); ++i) {
        int w;
        if (i + 1 == t.widths.size()) w = avail - used;
        else w = (int)((double)avail * (double)t.widths[i] / (double)total);
        if (w < 1) w = 1;
        cols.push_back(w);
        used += w;
    }
    return cols;
}

void drawTable(Gdiplus::Graphics& g, const Table& t) {
    RECT rr = t.rowsRect();
    RECT hr = t.headerRect();
    // 表头
    fillRect(g, Gdiplus::RectF((Gdiplus::REAL)hr.left, (Gdiplus::REAL)hr.top,
                               (Gdiplus::REAL)(t.rc.right - hr.left),
                               (Gdiplus::REAL)(hr.bottom - hr.top)), C_PANEL);
    std::vector<int> cols = tableColumns(t);
    Gdiplus::Font* fh = fontPt(10, true);
    Gdiplus::Font* fr = fontPt(10, false);
    int x = hr.left;
    for (size_t i = 0; i < t.headers.size() && i < cols.size(); ++i) {
        Gdiplus::RectF rc((Gdiplus::REAL)(x + 8), (Gdiplus::REAL)hr.top, (Gdiplus::REAL)(cols[i] - 12),
                          (Gdiplus::REAL)(hr.bottom - hr.top));
        drawText(g, t.headers[i], fh, C_SUB, rc, 0, 1, false, true);
        x += cols[i];
    }
    drawHDivider(g, hr.left, hr.bottom - 1, t.rc.right, C_BORDER);
    // 行区
    RECT body = t.bodyRect();
    fillRect(g, Gdiplus::RectF((Gdiplus::REAL)body.left, (Gdiplus::REAL)body.top,
                               (Gdiplus::REAL)(body.right - body.left),
                               (Gdiplus::REAL)(body.bottom - body.top)), C_PANEL);
    int viewH = (int)(rr.bottom - rr.top);
    int first = t.rowH > 0 ? t.offset / t.rowH : 0;
    for (int i = first; i < (int)t.rows.size(); ++i) {
        int y = rr.top + i * t.rowH - t.offset;
        if (y >= rr.bottom) break;
        if (y + t.rowH <= rr.top) continue;
        Gdiplus::RectF row((Gdiplus::REAL)rr.left, (Gdiplus::REAL)y, (Gdiplus::REAL)(rr.right - rr.left),
                           (Gdiplus::REAL)t.rowH);
        if (i == t.sel) fillRect(g, row, C_SELECTED);
        int cx = rr.left;
        const std::vector<std::wstring>& vals = t.rows[(size_t)i];
        for (size_t c = 0; c < cols.size(); ++c) {
            std::wstring s = c < vals.size() ? vals[c] : std::wstring();
            Gdiplus::RectF cell((Gdiplus::REAL)(cx + 8), (Gdiplus::REAL)y,
                                (Gdiplus::REAL)(cols[c] - 12), (Gdiplus::REAL)t.rowH);
            drawText(g, s, fr, C_TEXT, cell, 0, 1, false, true);
            cx += cols[c];
        }
    }
    // 滚动条
    if (t.withScrollbar) {
        RECT track{t.rc.right - TABLE_SCROLL_W, body.top, t.rc.right, body.bottom};
        drawScrollBar(g, track, t.contentH(), viewH, t.offset, false);
    }
}

// --------------------------------------------------------------- 投票卡 --
Card cardFromJson(const nw::Json& j) {
    Card c;
    c.req_id = j.get("req_id").asStr("");
    c.state = j.get("state").asStr("");
    c.note = j.get("note").asStr("");
    c.mac = j.get("mac").asStr("");
    c.ip = j.get("ip").asStr("");
    c.name = U8(j.get("name").asStr("?"));
    c.mac_pretty = U8(j.get("mac_pretty").asStr(""));
    if (c.mac_pretty.empty()) c.mac_pretty = fmtMacW(c.mac);
    c.desc = U8(j.get("desc").asStr(""));
    c.approvals = (int)j.get("approvals").asInt(0);
    c.rejects = (int)j.get("rejects").asInt(0);
    c.abstains = (int)j.get("abstains").asInt(0);
    c.voters = (int)j.get("voters").asInt(0);
    c.need = (int)j.get("need").asInt(0);
    c.from_me = j.get("from_me").asBool(false);
    c.voted = j.get("voted").asBool(false);
    c.can_change = j.get("can_change").asBool(false);
    c.remain = j.get("remain").asNum(0);
    c.change_left = j.get("change_left").asNum(0);
    nw::Json ch = j.get("my_choice");
    if (ch.isNull()) c.my_choice = 99;
    else if (ch.isBool()) c.my_choice = ch.asBool() ? 1 : 0;
    else c.my_choice = 99;
    return c;
}

bool cardCanVote(const Card& c, nw::PeerCore* core) {
    if (!core) return false;
    if (c.state != "pending" || c.from_me) return false;
    if (!core->in_net()) return false;
    return core->net_reqs.find(c.req_id) != core->net_reqs.end();
}

static std::wstring cardStateText(const std::string& st) {
    if (st == "pending") return L"投票中";
    if (st == "approved") return L"已通过";
    if (st == "rejected") return L"未通过";
    return U8(st);
}

int cardHeight(Gdiplus::Graphics& g, const Card& c, int width, nw::PeerCore* /*core*/) {
    int h = 10 + 4;                                     // top padx/pady=(10,4)
    h += (int)measureText(g, L"「」申请加入内网", fontPt(11, true)).Height + 4;
    h += (int)measureText(g, L"物理地址", fontPt(9, false)).Height;   // 信息行
    h += 2 + (int)measureText(g, L"规则", fontPt(8, false)).Height;
    if (c.state == "pending") h += 2 + (int)measureText(g, L"投票剩余", fontPt(8, false)).Height + 6;
    h += 4 + 22 + 12;                                   // 按钮行
    (void)width;
    return h;
}

void drawVoteCard(Gdiplus::Graphics& g, const RECT& rc, const Card& c, std::vector<Hot>& hits,
                  nw::PeerCore* core) {
    Gdiplus::RectF bg((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                      (Gdiplus::REAL)(rc.right - rc.left), (Gdiplus::REAL)(rc.bottom - rc.top));
    fillRect(g, bg, C_PANEL);
    int x = rc.left + 14;
    int w = (rc.right - rc.left) - 28;
    int y = rc.top + 10;
    Gdiplus::Font* f11b = fontPt(11, true);
    Gdiplus::Font* f9 = fontPt(9, false);
    Gdiplus::Font* f9b = fontPt(9, true);
    Gdiplus::Font* f8 = fontPt(8, false);
    // 第 1 行
    std::wstring title = L"「" + c.name + L"」申请加入内网";
    Gdiplus::RectF tr((Gdiplus::REAL)x, (Gdiplus::REAL)y, (Gdiplus::REAL)w, 20.f);
    drawText(g, title, f11b, C_TEXT, tr, 0, 1, false, true);
    float tw = measureText(g, title, f11b).Width;
    int sx = x + (int)tw + 8;
    std::wstring st = cardStateText(c.state);
    Gdiplus::Color stc = c.state == "approved" ? C_ACCENT : (c.state == "rejected" ? C_BADGE : C_SUB);
    Gdiplus::RectF sr((Gdiplus::REAL)sx, (Gdiplus::REAL)y, 100.f, 20.f);
    drawText(g, st, f9b, stc, sr, 0, 1, false, false);
    if (c.from_me) {
        float sw = measureText(g, st, f9b).Width;
        Gdiplus::RectF mr((Gdiplus::REAL)(sx + (int)sw + 8), (Gdiplus::REAL)y, 120.f, 20.f);
        drawText(g, L"（我自己）", f9, C_SUB, mr, 0, 1, false, false);
    }
    y += (int)measureText(g, title, f11b).Height + 4;
    // 第 2 行：信息
    wchar_t buf[512];
    swprintf(buf, 512, L"物理地址 %s · 内网 IP %s · 同意 %d/%d 人（需 %d 票） · 弃权 %d · 反对 %d",
             c.mac_pretty.c_str(), U8(c.ip.empty() ? "-" : c.ip).c_str(), c.approvals, c.voters,
             c.need, c.abstains, c.rejects);
    Gdiplus::RectF ir((Gdiplus::REAL)x, (Gdiplus::REAL)y, (Gdiplus::REAL)w, 20.f);
    drawText(g, buf, f9, C_SUB, ir, 0, 1, false, true);
    y += (int)measureText(g, L"物理", f9).Height;
    // 第 3 行：规则
    Gdiplus::RectF rr2((Gdiplus::REAL)x, (Gdiplus::REAL)(y + 2), (Gdiplus::REAL)w, 20.f);
    drawText(g, L"规则：" + c.desc, f8, C_SUB, rr2, 0, 1, false, true);
    y += 2 + (int)measureText(g, L"规则", f8).Height;
    if (c.state == "pending") {
        Gdiplus::RectF rm((Gdiplus::REAL)x, (Gdiplus::REAL)(y + 2), (Gdiplus::REAL)w, 20.f);
        drawText(g, L"投票剩余 " + fmtRemainW(c.remain), f8, C_SUB, rm, 0, 1, false, false);
        y += 2 + (int)measureText(g, L"投票剩余", f8).Height + 6;
    }
    // 第 5 行：按钮
    int by = y + 4;
    int bh = 22;
    bool can = cardCanVote(c, core);
    if (can) {
        if (!c.voted) {
            std::vector<std::wstring> labels = {L"同意", L"拒绝", L"弃权"};
            static const int vals[3] = {1, 0, -1};
            int cx = x;
            for (int i = 0; i < 3; ++i) {
                int bw = (int)measureText(g, labels[i], f9b).Width + 24;
                Gdiplus::RectF br((Gdiplus::REAL)cx, (Gdiplus::REAL)by, (Gdiplus::REAL)bw, (Gdiplus::REAL)bh);
                if (i == 0) {
                    fillRound(g, br, 3.f, C_ACCENT);
                    drawText(g, labels[i], f9b, C_WHITE, br, 1, 1, false, false);
                } else {
                    fillRound(g, br, 3.f, C_PANEL);
                    strokeRound(g, br, 3.f, i == 1 ? C_BADGE : RGBc(0xd0d0d0), 1.f);
                    drawText(g, labels[i], f9b, i == 1 ? C_BADGE : C_SUB, br, 1, 1, false, false);
                }
                Hot hh;
                hh.rc = RECT{cx, by, cx + bw, by + bh};
                hh.act = A_VOTE_CAST;
                hh.arg = c.req_id;
                hh.val = vals[i];
                hh.enabled = true;
                hh.hoverable = true;
                hits.push_back(hh);
                cx += bw + 6;
            }
        } else {
            const wchar_t* cur = c.my_choice == 1 ? L"同意" : (c.my_choice == 0 ? L"拒绝" : L"弃权");
            wchar_t b2[128];
            swprintf(b2, 128, L"我已投：%s", cur);
            Gdiplus::RectF lr((Gdiplus::REAL)x, (Gdiplus::REAL)by, 200.f, (Gdiplus::REAL)bh);
            drawText(g, b2, f9b, C_TEXT, lr, 0, 1, false, false);
            int cx = x + (int)measureText(g, b2, f9b).Width + 10;
            if (c.change_left > 0) {
                std::wstring hint = L"（" + fmtRemainW(c.change_left) + L"内可改票）";
                Gdiplus::RectF hr((Gdiplus::REAL)cx, (Gdiplus::REAL)by, 220.f, (Gdiplus::REAL)bh);
                drawText(g, hint, f8, C_SUB, hr, 0, 1, false, false);
                cx += (int)measureText(g, hint, f8).Width + 8;
                std::vector<std::wstring> labels = {L"改同意", L"改拒绝", L"改弃权"};
                static const int vals[3] = {1, 0, -1};
                static const Gdiplus::Color fgs[3] = {C_ACCENT, C_BADGE, C_SUB};
                for (int i = 0; i < 3; ++i) {
                    int bw = (int)measureText(g, labels[i], f9b).Width + 24;
                    Gdiplus::RectF br((Gdiplus::REAL)cx, (Gdiplus::REAL)by, (Gdiplus::REAL)bw,
                                      (Gdiplus::REAL)bh);
                    fillRound(g, br, 3.f, C_PANEL);
                    strokeRound(g, br, 3.f, RGBc(0xd0d0d0), 1.f);
                    drawText(g, labels[i], f9b, fgs[i], br, 1, 1, false, false);
                    Hot hh;
                    hh.rc = RECT{cx, by, cx + bw, by + bh};
                    hh.act = A_VOTE_CAST;
                    hh.arg = c.req_id;
                    hh.val = vals[i];
                    hh.enabled = true;
                    hh.hoverable = true;
                    hits.push_back(hh);
                    cx += bw + 6;
                }
            } else {
                Gdiplus::RectF hr((Gdiplus::REAL)cx, (Gdiplus::REAL)by, 260.f, (Gdiplus::REAL)bh);
                drawText(g, L"（已超过 5 分钟，不能再改票）", f8, C_SUB, hr, 0, 1, false, false);
            }
        }
    } else {
        std::wstring msg = c.note.empty() ? U8(c.note) : U8(c.note);
        if (msg.empty()) msg = L"等待成员投票…";
        std::vector<std::wstring> lines = wrapText(g, msg, f9, 560.f);
        int ly = by;
        for (auto& ln : lines) {
            Gdiplus::RectF lr((Gdiplus::REAL)x, (Gdiplus::REAL)ly, 560.f, 18.f);
            drawText(g, ln, f9, C_SUB, lr, 0, 1, false, false);
            ly += (int)measureText(g, L"字", f9).Height;
        }
    }
}

// ------------------------------------------------------------ 双缓冲 --
bool DblBuffer::begin(HDC target, int width, int height) {    if (width <= 0 || height <= 0) return false;
    release();
    hdc = target;
    w = width;
    h = height;
    mem = CreateCompatibleDC(target);
    if (!mem) return false;
    bmp = CreateCompatibleBitmap(target, w, h);
    if (!bmp) { DeleteDC(mem); mem = nullptr; return false; }
    old = (HBITMAP)SelectObject(mem, bmp);
    return true;
}

void DblBuffer::end() {
    if (mem && hdc) BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
}

void DblBuffer::release() {
    if (mem && old) SelectObject(mem, old);
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    mem = nullptr;
    bmp = nullptr;
    old = nullptr;
}

} // namespace ui
