// app_pages.cpp —— 主窗口的数据模型与五个页面的绘制
#include "app.h"

#include "forms.h"

#include <cstdio>

using namespace Gdiplus;

namespace ui {

int g_textH(int pt, bool bold);
int g_textW(const std::wstring& s, int pt, bool bold);

static void drawBtn(Graphics& g, const RECT& rc, const std::wstring& text, int pt, bool bold,
                    bool primary, bool outline, bool hover, const Gdiplus::Color& bgIn,
                    const Gdiplus::Color& fgIn) {
    drawStdButton(g, rc, text, pt, bold, primary, outline, hover, false, bgIn, fgIn);
}

// ------------------------------------------------------------ 基础访问 --
std::wstring App::myName() const {
    if (!core_) return std::wstring();
    return U8(core_->my_name());
}

std::wstring App::gnameOf(const std::string& gid) {
    if (!core_ || gid.empty()) return std::wstring();
    auto it = core_->groups.find(gid);
    if (it == core_->groups.end()) return std::wstring();
    return U8(it->second.get("gname").asStr(""));
}

bool App::inNet() { return core_ && core_->in_net(); }

std::vector<std::string> App::filteredGroups() const {
    std::vector<std::string> out;
    if (!core_) return out;
    std::wstring q;
    if (!searchPh_ && !search_.placeholder) q = lowerW(trimW(search_.text));
    for (auto& kv : core_->groups) {
        std::wstring gname = U8(kv.second.get("gname").asStr(""));
        std::wstring gid = U8(kv.first);
        if (!q.empty()) {
            if (lowerW(gname).find(q) == std::wstring::npos &&
                lowerW(gid).find(q) == std::wstring::npos)
                continue;
        }
        out.push_back(kv.first);
    }
    return out;
}

void App::updateSearchPlaceholder() {
    if (searchPh_) {
        search_.setPlaceholder(L"\U0001F50D  搜索群组");
    }
}

// ---------------------------------------------------------------- 日志 --
void App::appendLog(const std::wstring& line) {
    logLines_.push_back(line);
    int maxLines = core_ ? (int)core_->cfg.get("log_max_lines").asInt(800) : 800;
    if (maxLines <= 0) maxLines = 800;
    while ((int)logLines_.size() > maxLines) logLines_.erase(logLines_.begin());
    if (nav_ == "log") {
        scLog_.contentH = (int)logLines_.size() * (g_textH(9, false) + 2);
        scLog_.offset = scLog_.maxOffset();      // see("end")
    }
    invalidate();
}

void App::clearLog() {
    logLines_.clear();
    scLog_.contentH = 0;
    scLog_.offset = 0;
    invalidate();
}

// ---------------------------------------------------------------- 刷新 --
void App::refreshGroupsList() {
    if (!core_) return;
    std::vector<std::string> gids = filteredGroups();
    scGroups_.contentH = (int)gids.size() * 62 + 8;
    scGroups_.clamp();
    if (!currentGid_.empty() && core_->groups.find(currentGid_) == core_->groups.end()) {
        currentGid_.clear();
        renderedN_ = 0;
        chatRowGid_.clear();
        chatRowN_ = 0;
    }
    updateGroupHeader();
    if (nav_ == "chat") renderChat(false);
    invalidate();
}

void App::refreshPeers() {
    if (!core_) return;
    double now = nw::now_ts();
    int online = 0;
    for (auto& kv : core_->peers) {
        double ts = kv.second.ts;
        if (now - (ts > 0 ? ts : 0) <= 30.0) ++online;
    }
    std::wstring netTxt;
    if (core_->in_net()) {
        nw::Json v = core_->net_view();
        wchar_t buf[256];
        swprintf(buf, 256, L"内网「%s」%d 人", U8(v.get("name").asStr("")).c_str(),
                 (int)v.get("member_count").asInt(0));
        netTxt = buf;
    } else {
        netTxt = L"未加入内网（正在扫描）";
    }
    wchar_t buf[256];
    swprintf(buf, 256, L"%s · 在线用户 %d · v%S", netTxt.c_str(), online,
             nw::APP_VERSION_FALLBACK);
    statusLine_ = buf;
    if (!currentGid_.empty()) updateGroupHeader();
    for (auto& kv : dmWindows_) {
        if (kv.second) kv.second->refreshStatus();
    }
    invalidate();
}

void App::updateGroupHeader() {
    invalidate();
}

void App::updateVoteBadge() {
    if (!core_) return;
    size_t n = core_->net_pending_for_me().size();
    voteDotOn_ = (n > 0);
    invalidate();
}

void App::selectGid(const std::string& gid) {
    if (!core_ || core_->groups.find(gid) == core_->groups.end()) return;
    currentGid_ = gid;
    unread_[gid] = 0;
    renderedN_ = 0;
    chatRowGid_.clear();
    chatRowN_ = 0;
    renderChat(false);
    renderFiles();
    renderMembers();
    updateGroupHeader();
    setChip(chip_);
    invalidate();
}

void App::switchNav(const std::string& key) {
    nav_ = key;
    if (key == "net") renderNetPage();
    else if (key == "vote") renderVotePage();
    else if (key == "more") renderMorePage();
    else if (key == "chat") {
        setChip(chip_);
        scrollBottomLater();
    } else if (key != "log") {
        nav_ = "chat";
    }
    invalidate();
}

void App::setChip(const std::string& key) {
    chip_ = key;
    if (chip_ == "files") renderFiles();
    else if (chip_ == "members") renderMembers();
    else scrollBottomLater();
    invalidate();
}

// 切回聊天页时 60ms 后滚到底（scroll_bottom_later）
void App::scrollBottomLater() {
    post([this]() {
        scChat_.offset = scChat_.maxOffset();
        invalidate();
    }, 60);
}

// ------------------------------------------------------------ 聊天页 --
void App::pushChatMsg(const std::string& gid, const ChatMsg& m) {
    std::vector<ChatMsg>& v = chatCache_[gid];
    v.push_back(m);
    if ((int)v.size() > chatCacheMax_) v.erase(v.begin(), v.begin() + (v.size() - chatCacheMax_));
}

void App::renderChat(bool append) {
    if (!append) {
        renderedN_ = 0;
        chatRowGid_.clear();
    }
    std::vector<ChatMsg>& v = chatCache_[currentGid_];
    renderedN_ = v.size();
    invalidate();
}

void App::renderFiles() {
    if (!core_) return;
    tFiles_.headers = {L"文件名", L"大小", L"发送人", L"状态"};
    tFiles_.widths = {320, 100, 130, 160};
    tFiles_.rowH = 34;
    tFiles_.withScrollbar = true;
    tFiles_.rows.clear();
    tFiles_.iids.clear();
    if (!tFiles_.rc.right) tFiles_.rc = RECT{364, 100, 900, 500};
    auto it = fileModel_.find(currentGid_);
    if (it != fileModel_.end()) {
        std::vector<const FileRow*> list;
        for (auto& kv : it->second) list.push_back(&kv.second);
        // 排序键：(status == "已下载", -size)：未下载的排前面；同组内按大小从大到小
        std::sort(list.begin(), list.end(), [](const FileRow* a, const FileRow* b) {
            bool ad = (a->status == L"已下载");
            bool bd = (b->status == L"已下载");
            if (ad != bd) return !ad;
            return a->size > b->size;
        });
        for (auto* r : list) {
            std::vector<std::wstring> row;
            row.push_back(r->name);
            row.push_back(humanSizeW(r->size));
            row.push_back(r->sender);
            row.push_back(r->status);
            tFiles_.rows.push_back(row);
            tFiles_.iids.push_back(r->fid);
        }
    }
    // 选中行按 id 还原
    tFiles_.sel = -1;
    if (!selectedFileId_.empty()) {
        for (size_t i = 0; i < tFiles_.iids.size(); ++i) {
            if (tFiles_.iids[i] == selectedFileId_) { tFiles_.sel = (int)i; break; }
        }
    }
    invalidate();
}

void App::renderMembers() { invalidate(); }
void App::renderNetPage() { invalidate(); }
void App::renderVotePage() { invalidate(); }
void App::renderMorePage() { invalidate(); }

// 计算聊天行高（缓存）
static int chatRowHeight(Graphics& g, const ChatMsg& m, int width) {
    if (m.sys) return g_textH(9, false) + 6;
    Gdiplus::Font* f = fontPt(10, false);
    std::vector<std::wstring> lines = wrapText(g, m.text, f, 430.f - 24.f);
    int th = g_textH(10, false);
    int bh = (int)lines.size() * th + 16;
    int h = bh;
    if (!m.self) h += 30 + 2;         // 头像行
    h += 2 + g_textH(8, false);       // 时间戳
    return h + 10;
}

void App::paintChatView(Graphics& g, const RECT& box) {
    // 输入区（贴底）
    RECT inp{box.left, box.bottom, box.right, box.bottom};
    int toolH = 6 + g_textH(10, false) + 2;
    int entryH = g_textH(10, false) + 16;
    int rowH = (std::max)(entryH, g_textH(10, true) + 16) + 12;
    int totalInp = 1 + toolH + rowH;
    inp.top = box.bottom - totalInp;
    RECT sendRect{};
    {
        fillRect(g, Gdiplus::RectF((REAL)inp.left, (REAL)inp.top, (REAL)(inp.right - inp.left),
                                   (REAL)(inp.bottom - inp.top)), C_PANEL);
        drawHDivider(g, inp.left, inp.top, inp.right, C_BORDER);
        RECT tool{inp.left + 12, inp.top + 1 + 6, inp.right - 12, inp.top + 1 + 6 + g_textH(10, false) + 4};
        RECT tb{tool.left, tool.top, tool.left + g_textW(L"📎  发送文件", 10, false) + 8, tool.bottom};
        drawText(g, L"📎  发送文件", fontPt(10, false), C_SUB,
                 Gdiplus::RectF((REAL)tb.left, (REAL)tb.top, (REAL)(tb.right - tb.left + 40),
                                (REAL)(tb.bottom - tb.top)),
                 0, 1, false, false);
        Hot hs;
        hs.rc = RECT{tb.left, tb.top, tb.left + g_textW(L"发送文件", 10, false) + 40, tb.bottom};
        hs.act = A_SEND_FILE;
        hits_.push_back(hs);

        int ry = inp.top + 1 + toolH;
        int sendW = g_textW(L"发送", 10, true) + 18;
        int sendH = g_textH(10, true) + 16;
        sendRect = RECT{inp.right - 12 - sendW, ry, inp.right - 12, ry + sendH};
        RECT er{inp.left + 12, ry, sendRect.left - 10, ry + sendH};
        fillRect(g, Gdiplus::RectF((REAL)er.left, (REAL)er.top, (REAL)(er.right - er.left),
                                   (REAL)(er.bottom - er.top)), C_BG);
        drawText(g, chatEntry_.text, fontPt(10, false), C_TEXT,
                 Gdiplus::RectF((REAL)(er.left + 6), (REAL)er.top, (REAL)(er.right - er.left - 12),
                                (REAL)(er.bottom - er.top)),
                 0, 1, false, true);
        if (chatEntry_.focused && caretOn_) {
            int cx = chatEntry_.caretX(measureG(), fontPt(10, false), er.left + 6);
            drawVDivider(g, cx, er.top + 4, er.bottom - 4, C_TEXT);
        }
        Hot he;
        he.rc = er;
        he.act = A_CHAT_ENTRY;
        hits_.push_back(he);
        drawBtn(g, sendRect, L"发送", 10, true, true, false, hoverAct_ == A_SEND_CHAT, C_ACCENT,
                C_WHITE);
        Hot hb;
        hb.rc = sendRect;
        hb.act = A_SEND_CHAT;
        hits_.push_back(hb);
    }
    RECT list{box.left, box.top, box.right, inp.top};
    scChat_.rc = list;
    fillRect(g, Gdiplus::RectF((REAL)list.left, (REAL)list.top, (REAL)(list.right - list.left),
                               (REAL)(list.bottom - list.top)), C_BG);
    std::vector<ChatMsg>& msgs = chatCache_[currentGid_];
    int width = list.right - list.left;
    if (chatRowGid_ != currentGid_ || chatRowN_ != msgs.size() || chatRowW_ != width) {
        chatRowH_.clear();
        chatRowH_.reserve(msgs.size());
        for (auto& m : msgs) chatRowH_.push_back(chatRowHeight(g, m, width));
        chatRowGid_ = currentGid_;
        chatRowN_ = msgs.size();
        chatRowW_ = width;
    }
    int contentH = 0;
    for (int h : chatRowH_) contentH += h;
    scChat_.contentH = contentH;
    scChat_.clamp();
    if (scChat_.contentH > scChat_.viewH()) {
        RECT track{list.right - SCROLLBAR_W, list.top, list.right, list.bottom};
        drawScrollBar(g, track, scChat_.contentH, scChat_.viewH(), scChat_.offset, false);
        Track t{&scChat_, track};
        tracks_.push_back(t);
        list.right -= SCROLLBAR_W;
    }
    int y = list.top - scChat_.offset;
    for (size_t i = 0; i < msgs.size(); ++i) {
        int rowH = chatRowH_[i];
        int top = y;
        y += rowH;
        if (top + rowH < list.top || top > list.bottom) continue;
        const ChatMsg& m = msgs[i];
        if (m.sys) {
            drawText(g, L"── " + m.text + L" ──", fontPt(9, false), C_SUB,
                     Gdiplus::RectF((REAL)list.left, (REAL)(top + 3), (REAL)(list.right - list.left),
                                    (REAL)g_textH(9, false)),
                     1, 1, false, false);
            continue;
        }
        int x0 = list.left + 16;
        int x1 = list.right - 16;
        int by = top + 5;
        if (m.self) {
            Gdiplus::Font* f = fontPt(10, false);
            std::vector<std::wstring> lines = wrapText(g, m.text, f, 430.f - 24.f);
            int th = g_textH(10, false);
            int bw = 0;
            for (auto& ln : lines) bw = (std::max)(bw, g_textW(ln, 10, false));
            int bh = (int)lines.size() * th + 16;
            int brx = x1 - (bw + 24);
            Gdiplus::RectF bub((REAL)brx, (REAL)by, (REAL)(bw + 24), (REAL)bh);
            fillRound(g, bub, 4.f, C_BUBBLE_SELF);
            int ly = by + 8;
            for (auto& ln : lines) {
                drawText(g, ln, f, C_TEXT,
                         Gdiplus::RectF((REAL)(brx + 12), (REAL)ly, (REAL)(bw + 4), (REAL)th), 0, 1,
                         false, false);
                ly += th;
            }
            std::wstring ts = timeHMS(m.ts) + L"  我";
            drawText(g, ts, fontPt(8, false), C_SUB,
                     Gdiplus::RectF((REAL)x0, (REAL)(by + bh + 2), (REAL)(x1 - x0),
                                    (REAL)g_textH(8, false)),
                     2, 1, false, false);
        } else {
            RECT av{list.left + 16, by, list.left + 16 + 30, by + 30};
            drawAvatar(g, (REAL)av.left, (REAL)av.top, 30, m.from);
            int nx = av.right + 6;
            drawText(g, m.from, fontPt(8, true), C_SUB,
                     Gdiplus::RectF((REAL)nx, (REAL)by, 200, (REAL)g_textH(8, true)), 0, 1, false,
                     true);
            drawText(g, timeHMS(m.ts), fontPt(8, false), C_SUB,
                     Gdiplus::RectF((REAL)nx, (REAL)(by + g_textH(8, true)), 200,
                                    (REAL)g_textH(8, false)),
                     0, 1, false, false);
            int byy = by + 30 + 2;
            Gdiplus::Font* f = fontPt(10, false);
            std::vector<std::wstring> lines = wrapText(g, m.text, f, 430.f - 24.f);
            int th = g_textH(10, false);
            int bw = 0;
            for (auto& ln : lines) bw = (std::max)(bw, g_textW(ln, 10, false));
            int bh = (int)lines.size() * th + 16;
            int bx = list.left + 16;
            Gdiplus::RectF bub((REAL)bx, (REAL)byy, (REAL)(bw + 24), (REAL)bh);
            fillRound(g, bub, 4.f, C_BUBBLE_OTHER);
            strokeRound(g, bub, 4.f, C_BORDER, 1.f);
            int ly = byy + 8;
            for (auto& ln : lines) {
                drawText(g, ln, f, C_TEXT,
                         Gdiplus::RectF((REAL)(bx + 12), (REAL)ly, (REAL)(bw + 4), (REAL)th), 0, 1,
                         false, false);
                ly += th;
            }
            if (!m.from_uid.empty()) {
                Hot ha;
                ha.rc = av;
                ha.act = A_MEMBER_CARD;
                ha.arg = m.from_uid;
                hits_.push_back(ha);
                Hot hn;
                hn.rc = RECT{nx, by, nx + 200, by + g_textH(8, true) + g_textH(8, false)};
                hn.act = A_MEMBER_CARD;
                hn.arg = m.from_uid;
                hits_.push_back(hn);
            }
        }
    }
}

// ------------------------------------------------------------ 群文件页 --
void App::paintFilesView(Graphics& g, const RECT& box) {
    fillRect(g, Gdiplus::RectF((REAL)box.left, (REAL)box.top, (REAL)(box.right - box.left),
                               (REAL)(box.bottom - box.top)), C_BG);
    int barY = box.top + 10;
    int bh = g_textH(9, false) + 8;
    int bx = box.left + 14;
    const wchar_t* names[3] = {L"打开所选文件", L"重试下载", L"打开下载目录"};
    const int acts[3] = {A_FILE_OPEN, A_FILE_RETRY, A_FILE_OPENDIR};
    for (int i = 0; i < 3; ++i) {
        int w = g_textW(names[i], 9, false) + 14;
        RECT r{bx, barY, bx + w, barY + bh};
        drawBtn(g, r, names[i], 9, false, false, true, hoverAct_ == acts[i], C_PANEL, C_SUB);
        Hot h;
        h.rc = r;
        h.act = acts[i];
        hits_.push_back(h);
        bx += w + 8;
    }
    int ty = barY + bh + 10;
    tFiles_.rc = RECT{box.left + 14, ty, box.right, box.bottom - 10};
    tFiles_.rowH = 34;
    tFiles_.withScrollbar = true;
    if (tFiles_.rows.empty() && !fileModel_.empty()) renderFiles();
    drawTable(g, tFiles_);
    tables_.push_back(&tFiles_);
    {
        RECT rr = tFiles_.rowsRect();
        for (int i = 0; i < (int)tFiles_.rows.size(); ++i) {
            int ry = rr.top + i * tFiles_.rowH - tFiles_.offset;
            if (ry + tFiles_.rowH <= rr.top || ry >= rr.bottom) continue;
            Hot h;
            h.rc = RECT{rr.left, ry, rr.right, ry + tFiles_.rowH};
            h.act = A_FILE_ROW;
            h.arg = i < (int)tFiles_.iids.size() ? tFiles_.iids[(size_t)i] : std::string();
            hits_.push_back(h);
        }
    }
}

// ------------------------------------------------------------ 成员页 --
void App::paintMembersView(Graphics& g, const RECT& box) {
    fillRect(g, Gdiplus::RectF((REAL)box.left, (REAL)box.top, (REAL)(box.right - box.left),
                               (REAL)(box.bottom - box.top)), C_BG);
    scMembers_.rc = RECT{box.left + 14, box.top + 10, box.right - 14, box.bottom - 10};
    std::vector<nw::Json> members = currentGid_.empty() ? std::vector<nw::Json>()
                                                        : core_->group_members(currentGid_);
    int y = scMembers_.rc.top - scMembers_.offset;
    int hdrH = g_textH(9, false) + 6;
    drawText(g, L"", fontPt(9, false), C_SUB, Gdiplus::RectF(), 0, 1, false, false);
    y += hdrH;
    int cardH = (std::max)(39, 36 + 6) + 1;
    scMembers_.contentH = hdrH + (int)members.size() * (cardH + 6);
    scMembers_.clamp();
    if (scMembers_.contentH > scMembers_.viewH()) {
        RECT track{scMembers_.rc.right - SCROLLBAR_W, scMembers_.rc.top, scMembers_.rc.right,
                   scMembers_.rc.bottom};
        drawScrollBar(g, track, scMembers_.contentH, scMembers_.viewH(), scMembers_.offset, false);
        Track t{&scMembers_, track};
        tracks_.push_back(t);
    }
    wchar_t cbuf[128];
    swprintf(cbuf, 128, L"本群在线成员 · %d 人 · 点击成员头像可私聊", (int)members.size());
    drawText(g, cbuf, fontPt(9, false), C_SUB,
             Gdiplus::RectF((REAL)scMembers_.rc.left, (REAL)(scMembers_.rc.top - scMembers_.offset),
                            (REAL)(scMembers_.rc.right - scMembers_.rc.left),
                            (REAL)g_textH(9, false)),
             0, 1, false, false);
    for (size_t i = 0; i < members.size(); ++i) {
        const nw::Json& m = members[i];
        RECT card{scMembers_.rc.left, y, scMembers_.rc.right, y + cardH};
        y += cardH + 6;
        if (card.bottom < scMembers_.rc.top || card.top > scMembers_.rc.bottom) continue;
        fillRect(g, Gdiplus::RectF((REAL)card.left, (REAL)card.top, (REAL)(card.right - card.left),
                                   (REAL)(card.bottom - card.top)), C_PANEL);
        std::wstring nm = U8(m.get("name").asStr("?"));
        bool self = m.get("self").asBool(false);
        drawAvatar(g, (REAL)(card.left + 10), (REAL)(card.top + 2), 36, nm);
        int tx = card.left + 10 + 36 + 8;
        std::wstring title = nm + (self ? L"（我）" : L"");
        drawText(g, title, fontPt(10, true), C_TEXT,
                 Gdiplus::RectF((REAL)tx, (REAL)(card.top + 7), (REAL)(card.right - tx - 90),
                                (REAL)g_textH(10, true)),
                 0, 1, false, true);
        std::wstring ip = L"局域网地址 " + U8(m.get("ip").asStr(""));
        drawText(g, ip, fontPt(8, false), C_SUB,
                 Gdiplus::RectF((REAL)tx, (REAL)(card.top + 7 + g_textH(10, true)),
                                (REAL)(card.right - tx - 12), (REAL)g_textH(8, false)),
                 0, 1, false, true);
        if (self) {
            drawText(g, L"我自己", fontPt(8, false), C_SUB,
                     Gdiplus::RectF((REAL)(card.right - 100), (REAL)card.top, 90,
                                    (REAL)cardH),
                     2, 1, false, false);
        } else {
            drawText(g, L"私聊 →", fontPt(9, false), C_ACCENT,
                     Gdiplus::RectF((REAL)(card.right - 100), (REAL)card.top, 90,
                                    (REAL)cardH),
                     2, 1, false, false);
            Hot h;
            h.rc = card;
            h.act = A_MEMBER_CARD;
            h.arg = m.get("uid").asStr("");
            hits_.push_back(h);
        }
        drawHDivider(g, card.left, card.bottom - 1, card.right, C_BORDER);
    }
}

// ------------------------------------------------------------ 内网页 --
void App::paintNetPage(Graphics& g) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int W = cr.right, H = cr.bottom;
    int X0 = 364;
    fillRect(g, Gdiplus::RectF((REAL)X0, 0, (REAL)(W - X0), (REAL)H), C_BG);
    fillRect(g, Gdiplus::RectF((REAL)X0, 0, (REAL)(W - X0), 48), C_PANEL);
    drawHDivider(g, X0, 47, W, C_BORDER);
    drawText(g, L"内网", fontPt(13, true), C_TEXT,
             Gdiplus::RectF((REAL)(X0 + 18), 0, 60, 48), 0, 1, false, false);
    drawText(g, netStatusText_, fontPt(9, false), C_SUB,
             Gdiplus::RectF((REAL)(X0 + 18 + 44), 0, (REAL)(W - X0 - 100), 48), 0, 1, false, true);

    nw::Json v = core_->net_view();
    int bx = X0 + 16, by = 48 + 12;
    int bw = W - X0 - 32;
    // 我的内网信息卡
    int cardH = 12 + g_textH(13, true) + 2 + g_textH(9, false) + 10 + (g_textH(9, false) + 8) + 12;
    RECT card{bx, by, bx + bw, by + cardH};
    fillRect(g, Gdiplus::RectF((REAL)card.left, (REAL)card.top, (REAL)(card.right - card.left),
                               (REAL)(card.bottom - card.top)), C_PANEL);
    bool inNet = v.get("in_net").asBool(false);
    std::wstring title;
    std::wstring info;
    bool createEnabled = true, leaveEnabled = true, renameEnabled = true;
    if (inNet) {
        wchar_t tb[256];
        swprintf(tb, 256, L"我的内网：%s（编号 %s）", U8(v.get("name").asStr("")).c_str(),
                 U8(v.get("nid").asStr("")).c_str());
        title = tb;
        wchar_t ib[512];
        swprintf(ib, 512, L"成员 %d 人（当前在线 %d 人） · 我是%s · 我的物理地址 %s",
                 (int)v.get("member_count").asInt(0), (int)v.get("online_count").asInt(0),
                 v.get("is_founder").asBool(false) ? L"发起者" : L"成员",
                 U8(v.get("my_mac_pretty").asStr("")).c_str());
        info = ib;
        createEnabled = false;
        renameEnabled = v.get("is_founder").asBool(false);
    } else {
        nw::Json a = v.get("applying");
        if (a.isObj()) {
            std::wstring nm = U8(a.get("name").asStr(""));
            if (nm.empty()) nm = U8(a.get("nid").asStr(""));
            title = L"正在申请加入内网「" + nm + L"」";
            info = L"已提交内网申请，等待成员投票（有效期 1 天）。投票进度见左侧「投票」图标。";
        } else {
            title = L"尚未加入任何内网";
            info = L"程序会自动扫描附近的内网：有就申请加入，没有就自行建立一个。";
        }
        leaveEnabled = false;
        renameEnabled = false;
    }
    drawText(g, title, fontPt(13, true), C_TEXT,
             Gdiplus::RectF((REAL)(card.left + 16), (REAL)(card.top + 12),
                            (REAL)(card.right - card.left - 32), (REAL)g_textH(13, true)),
             0, 1, false, true);
    drawText(g, info, fontPt(9, false), C_SUB,
             Gdiplus::RectF((REAL)(card.left + 16),
                            (REAL)(card.top + 12 + g_textH(13, true) + 2),
                            (REAL)(card.right - card.left - 32), (REAL)g_textH(9, false)),
             0, 1, false, true);
    {
        int nby = card.bottom - 12 - (g_textH(9, false) + 8);
        int nx = card.left + 16;
        const wchar_t* names[4] = {L"重新扫描附近内网", L"自行建立内网", L"重命名", L"退出内网"};
        const int acts[4] = {A_NET_SCAN, A_NET_CREATE, A_NET_RENAME, A_NET_LEAVE};
        const bool en[4] = {true, createEnabled, renameEnabled, leaveEnabled};
        for (int i = 0; i < 4; ++i) {
            int w = g_textW(names[i], 9, false) + 22;
            int h = g_textH(9, false) + 8;
            RECT r{nx, nby, nx + w, nby + h};
            Gdiplus::RectF rf((REAL)r.left, (REAL)r.top, (REAL)w, (REAL)h);
            fillRect(g, rf, C_PANEL);
            if (en[i]) {
                Pen pen(C_BORDER, 1.f);
                g.DrawRectangle(&pen, (REAL)r.left + 0.5f, (REAL)r.top + 0.5f, (REAL)w - 1.f,
                                (REAL)h - 1.f);
            } else {
                Pen pen(RGBc(0xeeeeee), 1.f);
                g.DrawRectangle(&pen, (REAL)r.left + 0.5f, (REAL)r.top + 0.5f, (REAL)w - 1.f,
                                (REAL)h - 1.f);
            }
            drawText(g, names[i], fontPt(9, false), en[i] ? C_ACCENT : C_SUB, rf, 1, 1, false,
                     false);
            if (en[i]) {
                Hot h;
                h.rc = r;
                h.act = acts[i];
                hits_.push_back(h);
            }
            nx += w + 8;
        }
    }
    by = card.bottom;

    // 成员登记表
    drawText(g, L"成员登记表（物理地址 / 内网 IP）", fontPt(11, true), C_TEXT,
             Gdiplus::RectF((REAL)bx, (REAL)(by + 16), (REAL)bw, (REAL)g_textH(11, true)), 0, 1,
             false, false);
    int ty = by + 16 + g_textH(11, true) + 6;
    int th = tNetMembers_.rowH * 7 + tNetMembers_.rowH;      // height=7 行 + 表头
    tNetMembers_.rc = RECT{bx, ty, bx + bw, ty + th};
    tNetMembers_.rowH = 30;
    tNetMembers_.headers = {L"名字", L"物理地址（MAC）", L"内网 IP", L"加入时间", L"角色"};
    tNetMembers_.widths = {150, 190, 140, 140, 90};
    tNetMembers_.rows.clear();
    tNetMembers_.iids.clear();
    if (inNet) {
        std::string myMac = core_->uid;
        for (auto& m : v.get("members").items()) {
            std::vector<std::wstring> row;
            std::wstring nm = U8(m.get("name").asStr("?"));
            if (m.get("mac").asStr("") == myMac) nm += L"（我）";
            row.push_back(nm);
            row.push_back(fmtMacW(m.get("mac").asStr("")));
            std::string ip = m.get("ip").asStr("");
            row.push_back(ip.empty() ? L"-" : U8(ip));
            row.push_back(timeFull(m.get("joined_ts").asNum(0)));
            row.push_back(m.get("founder").asBool(false) ? L"发起者" : L"成员");
            tNetMembers_.rows.push_back(row);
            tNetMembers_.iids.push_back(m.get("mac").asStr(""));
        }
    }
    drawTable(g, tNetMembers_);
    tables_.push_back(&tNetMembers_);
    for (int i = 0; i < (int)tNetMembers_.rows.size(); ++i) {
        RECT rr = tNetMembers_.rowsRect();
        int ry = rr.top + i * tNetMembers_.rowH - tNetMembers_.offset;
        if (ry + tNetMembers_.rowH <= rr.top || ry >= rr.bottom) continue;
        Hot h;
        h.rc = RECT{rr.left, ry, rr.right, ry + tNetMembers_.rowH};
        h.act = A_NETMEMBER_ROW;
        hits_.push_back(h);
    }
    by = tNetMembers_.rc.bottom;

    // 附近的内网
    drawText(g, L"附近的内网（自动扫描发现）", fontPt(11, true), C_TEXT,
             Gdiplus::RectF((REAL)bx, (REAL)(by + 14), (REAL)bw, (REAL)g_textH(11, true)), 0, 1,
             false, false);
    ty = by + 14 + g_textH(11, true) + 6;
    th = tNearby_.rowH * 5 + tNearby_.rowH;
    tNearby_.rc = RECT{bx, ty, bx + bw, ty + th};
    tNearby_.rowH = 30;
    tNearby_.headers = {L"内网名称", L"内网编号", L"发起者物理地址", L"成员数"};
    tNearby_.widths = {220, 110, 190, 90};
    tNearby_.rows.clear();
    tNearby_.iids.clear();
    for (auto& n : v.get("nearby").items()) {
        std::vector<std::wstring> row;
        row.push_back(U8(n.get("name").asStr("")));
        row.push_back(U8(n.get("nid").asStr("")));
        row.push_back(fmtMacW(n.get("founder_mac").asStr("")));
        row.push_back(std::to_wstring(n.get("member_count").asInt(0)));
        tNearby_.rows.push_back(row);
        tNearby_.iids.push_back(n.get("nid").asStr(""));
    }
    drawTable(g, tNearby_);
    tables_.push_back(&tNearby_);
    for (int i = 0; i < (int)tNearby_.rows.size(); ++i) {
        RECT rr = tNearby_.rowsRect();
        int ry = rr.top + i * tNearby_.rowH - tNearby_.offset;
        if (ry + tNearby_.rowH <= rr.top || ry >= rr.bottom) continue;
        Hot h;
        h.rc = RECT{rr.left, ry, rr.right, ry + tNearby_.rowH};
        h.act = A_NEARBY_ROW;
        hits_.push_back(h);
    }
    by = tNearby_.rc.bottom + 6;
    {
        int w = g_textW(L"申请加入所选内网", 9, false) + 22;
        int h = g_textH(9, false) + 8;
        RECT r{bx, by, bx + w, by + h};
        drawBtn(g, r, L"申请加入所选内网", 9, false, false, true, hoverAct_ == A_NET_APPLY, C_PANEL,
                C_ACCENT);
        Hot hh;
        hh.rc = r;
        hh.act = A_NET_APPLY;
        hits_.push_back(hh);
        std::wstring lbl = netApplyLbl_;
        if (inNet && !v.get("nearby").items().empty()) lbl = L"（已在其他内网中，不能重复申请）";
        else if (v.get("nearby").items().empty()) lbl = L"附近暂未发现其它内网";
        if (!lbl.empty()) {
            drawText(g, lbl, fontPt(9, false), C_SUB,
                     Gdiplus::RectF((REAL)(r.right + 10), (REAL)by, 420, (REAL)h), 0, 1, false,
                     true);
        }
    }
}

// ------------------------------------------------------------ 投票页 --
void App::paintVotePage(Graphics& g) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int W = cr.right, H = cr.bottom;
    int X0 = 364;
    fillRect(g, Gdiplus::RectF((REAL)X0, 0, (REAL)(W - X0), (REAL)H), C_BG);
    fillRect(g, Gdiplus::RectF((REAL)X0, 0, (REAL)(W - X0), 48), C_PANEL);
    drawHDivider(g, X0, 47, W, C_BORDER);
    drawText(g, L"投票", fontPt(13, true), C_TEXT, Gdiplus::RectF((REAL)(X0 + 18), 0, 60, 48), 0,
             1, false, false);
    drawText(g, L"入网申请投票统一在这里处理（可弃权；投出后 5 分钟内可改票）", fontPt(9, false),
             C_SUB, Gdiplus::RectF((REAL)(X0 + 18 + 44), 0, (REAL)(W - X0 - 120), 48), 0, 1, false,
             true);
    scVote_.rc = RECT{X0 + 16, 48 + 12, W - 16, H - 12};
    int x = scVote_.rc.left;
    int width = scVote_.rc.right - scVote_.rc.left;
    int y = scVote_.rc.top - scVote_.offset;
    std::vector<nw::Json> cards = core_->net_pending_for_me();
    wchar_t buf[128];
    swprintf(buf, 128, L"待我投票（%d）", (int)cards.size());
    drawText(g, buf, fontPt(11, true), C_TEXT,
             Gdiplus::RectF((REAL)x, (REAL)y, (REAL)width, (REAL)g_textH(11, true)), 0, 1, false,
             false);
    y += g_textH(11, true) + 6;
    for (auto& cj : cards) {
        Card c = cardFromJson(cj);
        int ch = cardHeight(measureG(), c, width, core_);
        RECT rc{x, y, x + width, y + ch};
        drawVoteCard(g, rc, c, hits_, core_);
        Hot h;
        h.rc = rc;
        h.act = A_NONE;
        y += ch + 12;
    }
    std::set<std::string> pend;
    for (auto& cj : cards) pend.insert(cj.get("req_id").asStr(""));
    std::vector<nw::Json> opened;
    for (auto& cj : core_->net_open_votes()) {
        if (pend.find(cj.get("req_id").asStr("")) == pend.end()) opened.push_back(cj);
    }
    if (!opened.empty()) {
        y += 16;
        swprintf(buf, 128, L"其它进行中的投票（%d）", (int)opened.size());
        drawText(g, buf, fontPt(11, true), C_TEXT,
                 Gdiplus::RectF((REAL)x, (REAL)y, (REAL)width, (REAL)g_textH(11, true)), 0, 1,
                 false, false);
        y += g_textH(11, true) + 6;
        for (auto& cj : opened) {
            Card c = cardFromJson(cj);
            int ch = cardHeight(measureG(), c, width, core_);
            RECT rc{x, y, x + width, y + ch};
            drawVoteCard(g, rc, c, hits_, core_);
            y += ch + 12;
        }
    }
    nw::Json applying = core_->net_applying;
    if (applying.isObj()) {
        y += 16;
        drawText(g, L"我发起的入网申请", fontPt(11, true), C_TEXT,
                 Gdiplus::RectF((REAL)x, (REAL)y, (REAL)width, (REAL)g_textH(11, true)), 0, 1,
                 false, false);
        y += g_textH(11, true) + 6;
        std::string rid = applying.get("req_id").asStr("");
        for (auto& cj : core_->net_open_votes()) {
            if (cj.get("req_id").asStr("") != rid) continue;
            Card c = cardFromJson(cj);
            int ch = cardHeight(measureG(), c, width, core_);
            RECT rc{x, y, x + width, y + ch};
            drawVoteCard(g, rc, c, hits_, core_);
            y += ch + 12;
        }
        int w = g_textW(L"撤回申请", 9, false) + 22;
        int h = g_textH(9, false) + 8;
        RECT r{x, y + 8, x + w, y + 8 + h};
        drawBtn(g, r, L"撤回申请", 9, false, false, true, false, C_PANEL, C_ACCENT);
        Hot hh;
        hh.rc = r;
        hh.act = A_VOTE_CANCEL_APPLY;
        hits_.push_back(hh);
        y += 8 + h;
    }
    y += 16;
    const std::wstring note =
        L"说明：可弃权；投出后 5 分钟内可改票；一次投票有效期为 1 天，过期视为投票结束。"
        L"历史投票在左侧「更多」里查看。";
    std::vector<std::wstring> lines = wrapText(measureG(), note, fontPt(9, false), 760.f);
    for (auto& ln : lines) {
        drawText(g, ln, fontPt(9, false), C_SUB,
                 Gdiplus::RectF((REAL)x, (REAL)y, 760, (REAL)g_textH(9, false)), 0, 1, false,
                 false);
        y += g_textH(9, false);
    }
    scVote_.contentH = (y + scVote_.offset) - scVote_.rc.top + 12;
    scVote_.clamp();
    if (scVote_.contentH > scVote_.viewH()) {
        RECT track{scVote_.rc.right - SCROLLBAR_W, scVote_.rc.top, scVote_.rc.right,
                   scVote_.rc.bottom};
        drawScrollBar(g, track, scVote_.contentH, scVote_.viewH(), scVote_.offset, false);
        Track t{&scVote_, track};
        tracks_.push_back(t);
    }
}

// ------------------------------------------------------------ 更多页 --
void App::paintMorePage(Graphics& g) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int W = cr.right, H = cr.bottom;
    int X0 = 364;
    fillRect(g, Gdiplus::RectF((REAL)X0, 0, (REAL)(W - X0), (REAL)H), C_BG);
    fillRect(g, Gdiplus::RectF((REAL)X0, 0, (REAL)(W - X0), 48), C_PANEL);
    drawHDivider(g, X0, 47, W, C_BORDER);
    drawText(g, L"更多", fontPt(13, true), C_TEXT, Gdiplus::RectF((REAL)(X0 + 18), 0, 80, 48), 0,
             1, false, false);
    int bx = X0 + 16, by = 48 + 12;
    int bw = W - X0 - 32;
    drawText(g, L"投票历史（5 分钟内且投票未结束时可改票）", fontPt(11, true), C_TEXT,
             Gdiplus::RectF((REAL)bx, (REAL)by, (REAL)bw, (REAL)g_textH(11, true)), 0, 1, false,
             false);
    int ty = by + g_textH(11, true) + 6;
    int th = tHist_.rowH * 9 + tHist_.rowH;
    tHist_.rc = RECT{bx, ty, bx + bw, ty + th};
    tHist_.rowH = 30;
    tHist_.headers = {L"时间", L"申请人", L"物理地址", L"我的投票", L"状态", L"说明"};
    tHist_.widths = {130, 110, 180, 100, 100, 320};
    tHist_.rows.clear();
    tHist_.iids.clear();
    for (auto& cj : core_->net_vote_history()) {
        Card c = cardFromJson(cj);
        std::wstring my = c.my_choice == 1 ? L"同意" : (c.my_choice == 0 ? L"拒绝" : L"弃权");
        if (!c.voted) my = L"未投票";
        if (c.can_change) my += L"（可改）";
        std::wstring state = c.state == "pending" ? L"投票中"
                            : c.state == "approved" ? L"已通过"
                            : c.state == "rejected" ? L"未通过"
                                                    : U8(c.state);
        std::wstring note = U8(c.note);
        if (note.empty() && c.state == "pending") note = L"剩余 " + fmtRemainW(c.remain);
        std::vector<std::wstring> row;
        row.push_back(timeFull(cj.get("created_ts").asNum(0)));
        row.push_back(c.name);
        row.push_back(c.mac_pretty);
        row.push_back(my);
        row.push_back(state);
        row.push_back(note);
        tHist_.rows.push_back(row);
        tHist_.iids.push_back(c.req_id);
    }
    drawTable(g, tHist_);
    tables_.push_back(&tHist_);
    for (int i = 0; i < (int)tHist_.rows.size(); ++i) {
        RECT rr = tHist_.rowsRect();
        int ry = rr.top + i * tHist_.rowH - tHist_.offset;
        if (ry + tHist_.rowH <= rr.top || ry >= rr.bottom) continue;
        Hot h;
        h.rc = RECT{rr.left, ry, rr.right, ry + tHist_.rowH};
        h.act = A_HIST_ROW;
        h.arg = tHist_.iids[(size_t)i];
        hits_.push_back(h);
    }
    int hy = tHist_.rc.bottom + 6;
    {
        int w = g_textW(L"更改所选投票", 9, false) + 22;
        int h = g_textH(9, false) + 8;
        RECT r{bx, hy, bx + w, hy + h};
        drawBtn(g, r, L"更改所选投票", 9, false, false, true, hoverAct_ == A_CHANGE_VOTE, C_PANEL,
                C_ACCENT);
        Hot hh;
        hh.rc = r;
        hh.act = A_CHANGE_VOTE;
        hits_.push_back(hh);
        drawText(g, L"（只有 5 分钟内、且投票还没结束的才能改）", fontPt(8, false), C_SUB,
                 Gdiplus::RectF((REAL)(r.right + 8), (REAL)hy, 420, (REAL)h), 0, 1, false, true);
    }
    int ty2 = hy + g_textH(9, false) + 8 + 14;
    drawText(g, L"工具", fontPt(11, true), C_TEXT,
             Gdiplus::RectF((REAL)bx, (REAL)ty2, (REAL)bw, (REAL)g_textH(11, true)), 0, 1, false,
             false);
    int ty3 = ty2 + g_textH(11, true) + 6;
    const wchar_t* names[3] = {L"打开数据目录", L"检查更新", L"设置"};
    const int acts[3] = {A_TOOL_DATADIR, A_TOOL_UPDATE, A_TOOL_SETTINGS};
    int tx = bx;
    for (int i = 0; i < 3; ++i) {
        int w = g_textW(names[i], 9, false) + 22;
        int h = g_textH(9, false) + 8;
        RECT r{tx, ty3, tx + w, ty3 + h};
        drawBtn(g, r, names[i], 9, false, false, true, hoverAct_ == acts[i], C_PANEL, C_ACCENT);
        Hot hh;
        hh.rc = r;
        hh.act = acts[i];
        hits_.push_back(hh);
        tx += w + 8;
    }
}

// ------------------------------------------------------------ 日志页 --
void App::paintLogPage(Graphics& g) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int W = cr.right, H = cr.bottom;
    int X0 = 364;
    fillRect(g, Gdiplus::RectF((REAL)X0, 0, (REAL)(W - X0), (REAL)H), C_PANEL);
    fillRect(g, Gdiplus::RectF((REAL)X0, 0, (REAL)(W - X0), 48), C_PANEL);
    drawText(g, L"局域网公共日志（所有人可见 · 只读）", fontPt(13, true), C_TEXT,
             Gdiplus::RectF((REAL)(X0 + 18), 0, 520, 48), 0, 1, false, false);
    {
        int w = g_textW(L"清空本机显示", 9, false) + 10;
        int h = g_textH(9, false) + 8;
        RECT r{W - 14 - w, (48 - h) / 2, W - 14, (48 - h) / 2 + h};
        drawBtn(g, r, L"清空本机显示", 9, false, false, false, hoverAct_ == A_CLEAR_LOG, C_PANEL,
                C_SUB);
        Hot hh;
        hh.rc = r;
        hh.act = A_CLEAR_LOG;
        hits_.push_back(hh);
    }
    drawHDivider(g, X0, 48, W, C_BORDER);
    // design_v2 第 1.7 节：日志页底部一行「局域网活动」状态条
    int statusH = 26;
    RECT body{X0, 49, W, H - statusH};
    fillRect(g, Gdiplus::RectF((REAL)body.left, (REAL)body.top, (REAL)(body.right - body.left),
                               (REAL)(body.bottom - body.top)), C_LOG_BG);
    scLog_.rc = RECT{body.left, body.top, body.right - 14, body.bottom};
    Gdiplus::Font* f = monoFontPt(9);
    int lh = (int)measureText(measureG(), L"字", f).Height + 2;
    scLog_.contentH = (int)logLines_.size() * lh;
    scLog_.clamp();
    int y = scLog_.rc.top - scLog_.offset;
    int first = lh > 0 ? scLog_.offset / lh : 0;
    for (int i = first; i < (int)logLines_.size(); ++i) {
        int ly = scLog_.rc.top + i * lh - scLog_.offset;
        if (ly >= scLog_.rc.bottom) break;
        if (ly + lh <= scLog_.rc.top) continue;
        std::vector<std::wstring> lines = wrapText(measureG(), logLines_[(size_t)i], f,
                                                   (float)(scLog_.rc.right - scLog_.rc.left - 8));
        for (auto& ln : lines) {
            if (ly >= scLog_.rc.bottom) break;
            drawText(g, ln, f, C_TEXT,
                     Gdiplus::RectF((REAL)(scLog_.rc.left + 4), (REAL)ly,
                                    (REAL)(scLog_.rc.right - scLog_.rc.left - 8), (REAL)lh),
                     0, 1, false, true);
            ly += lh;
        }
        y = ly;
    }
    RECT track{body.right - 14, body.top, body.right, body.bottom};
    drawScrollBar(g, track, scLog_.contentH, scLog_.viewH(), scLog_.offset, false);
    Track t{&scLog_, track};
    tracks_.push_back(t);
    // 状态条：局域网活动：最后一次数据获取时间 … · 活动记录 N 条
    {
        RECT sb{X0, H - statusH, W, H};
        fillRect(g, Gdiplus::RectF((REAL)sb.left, (REAL)sb.top, (REAL)(sb.right - sb.left),
                                   (REAL)(sb.bottom - sb.top)), C_PANEL);
        drawHDivider(g, sb.left, sb.top, sb.right, C_BORDER);
        nw::Json act = core_->activity_view();
        double lastTs = act.get("last_get_ts").asNum(0);
        wchar_t buf[256];
        swprintf(buf, 256, L"局域网活动：最后一次数据获取时间 %s · 活动记录 %d 条",
                 lastTs > 0 ? timeFull(lastTs).c_str() : L"（无）",
                 (int)act.get("count").asInt(0));
        drawText(g, buf, fontPt(9, false), C_SUB,
                 Gdiplus::RectF((REAL)sb.left, (REAL)sb.top, (REAL)(sb.right - sb.left - 12),
                                (REAL)(sb.bottom - sb.top)),
                 2, 1, false, true);
    }
}

} // namespace ui
