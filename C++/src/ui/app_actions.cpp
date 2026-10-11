// app_actions.cpp —— 业务动作、事件处理、对话框粘合、投票悬浮面板
#include "app.h"

#include "forms.h"

#include "../common/util.h"

#include <commdlg.h>
#include <cstdio>
#include <thread>

#pragma comment(lib, "comdlg32.lib")

using namespace Gdiplus;

namespace ui {

int g_textH(int pt, bool bold);
int g_textW(const std::wstring& s, int pt, bool bold);

// ------------------------------------------------------------ 对话框入口 --
void App::openCreateGroup() {
    if (!core_->in_net()) {
        msgBox(this, L"提示", L"请先加入或建立一个内网，再创建群组。", 0, hwnd_);
        switchNav("net");
        return;
    }
    openCreateGroupDialog(this);
}

void App::openJoinGroup() {
    if (!core_->in_net()) {
        msgBox(this, L"提示", L"请先加入或建立一个内网，再加入群组。", 0, hwnd_);
        switchNav("net");
        return;
    }
    openJoinGroupDialog(this);
}

void App::openSettings() { openSettingsDialog(this); }

void App::openChangeVoteFromHistory() {
    if (selectedHistId_.empty()) {
        msgBox(this, L"提示", L"请先在投票历史里选一条记录", 0, hwnd_);
        return;
    }
    for (auto& cj : core_->net_vote_history()) {
        if (cj.get("req_id").asStr("") != selectedHistId_) continue;
        Card c = cardFromJson(cj);
        if (!c.can_change) {
            msgBox(this, L"提示",
                   L"这条投票不能改了：只有投票未结束、且投出后 5 分钟内的才能改。", 0, hwnd_);
            return;
        }
        openChangeVoteDialog(this, c);
        return;
    }
}

// ------------------------------------------------------------ 我的名字 --
void App::applyMyNameUi() {
    invalidate();
    updateGroupHeader();
}

// ------------------------------------------------------------ 私聊窗口 --
void App::registerDm(const std::string& uid, ChatWindow* w) { dmWindows_[uid] = w; }
void App::unregisterDm(const std::string& uid) { dmWindows_.erase(uid); }
ChatWindow* App::findDm(const std::string& uid) {
    auto it = dmWindows_.find(uid);
    if (it == dmWindows_.end()) return nullptr;
    if (!it->second || !it->second->hwnd()) return nullptr;
    return it->second;
}

void App::reapForm(Form* f) {
    if (!f) return;
    deadForms_.push_back(f);
}

void App::openDm(const std::string& uidIn) {
    std::string uid = uidIn;
    if (uid.empty() || uid == core_->uid) return;
    ChatWindow* w = findDm(uid);
    if (w) {
        HWND h = w->hwnd();
        if (h) {
            ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
        }
        return;
    }
    std::wstring name = L"用户" + U8(uid.substr(0, uid.size() < 6 ? uid.size() : 6));
    auto it = core_->peers.find(uid);
    if (it != core_->peers.end()) {
        std::wstring nm = U8(it->second.name);
        if (!nm.empty()) name = nm;
    }
    ChatWindow* cw = new ChatWindow(this, uid, name);
    if (!cw->createForm(inst_, hwnd_, this, L"私聊 · " + name, 440, 580, false)) {
        delete cw;
        return;
    }
    dmWindows_[uid] = cw;
}

// ------------------------------------------------------------ 路径/打开 --
void App::openDataDir() {
    std::wstring dir = U8(dataDir_);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::openDownloadsDir() {
    std::string dir = nw::path_join(dataDir_, "downloads");
    nw::ensure_dir(dir);
    ShellExecuteW(nullptr, L"open", U8(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::openFileByPath(const std::wstring& path, const std::wstring&) {
    if (path.empty()) return;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) msgBox(this, L"错误", L"无法用系统默认程序打开该文件", 2, hwnd_);
}

void App::openSelectedFile() {
    if (selectedFileId_.empty()) {
        msgBox(this, L"提示", L"请先在群文件列表中选择一个文件", 0, hwnd_);
        return;
    }
    FileRow* rec = nullptr;
    auto it = fileModel_.find(currentGid_);
    if (it != fileModel_.end()) {
        auto f = it->second.find(selectedFileId_);
        if (f != it->second.end()) rec = &f->second;
    }
    if (!rec) {
        msgBox(this, L"提示", L"该文件尚未下载到本机，正在等待自动下载完成。", 0, hwnd_);
        return;
    }
    std::wstring path = rec->path;
    if (path.empty() || !nw::file_exists(nw::ws2u8(path))) {
        std::wstring gname = gnameOf(currentGid_);
        std::string cand = nw::path_join(core_->downloads_dir(currentGid_, nw::ws2u8(gname)),
                                         nw::sanitize_name(nw::ws2u8(rec->name)));
        if (nw::file_exists(cand)) path = U8(cand);
    }
    if (path.empty() || !nw::file_exists(nw::ws2u8(path))) {
        msgBox(this, L"提示", L"该文件尚未下载到本机，正在等待自动下载完成。", 0, hwnd_);
        return;
    }
    openFileByPath(path, rec->name);
}

void App::retryDownload() {
    if (selectedFileId_.empty()) return;
    if (currentGid_.empty()) return;
    FileRow* rec = nullptr;
    auto it = fileModel_.find(currentGid_);
    if (it != fileModel_.end()) {
        auto f = it->second.find(selectedFileId_);
        if (f != it->second.end()) rec = &f->second;
    }
    if (!rec) return;
    nw::DlTask task;
    task.fid = rec->fid;
    task.name = nw::ws2u8(rec->name);
    task.size = rec->size;
    task.gid = currentGid_;
    task.gname = nw::ws2u8(gnameOf(currentGid_));
    nw::Json idx = core_->index_record(rec->fid);
    task.sender_uid = idx.isObj() ? idx.get("src_uid").asStr("") : std::string();
    task.sender_name = nw::ws2u8(rec->sender);
    {
        std::lock_guard<std::recursive_mutex> lk(core_->mtx);
        core_->download_queue.push_back(task);
    }
}

void App::sendFiles() {
    if (currentGid_.empty()) {
        msgBox(this, L"提示", L"请先选择或创建一个群组", 0, hwnd_);
        return;
    }
    wchar_t buf[32768];
    buf[0] = 0;
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"所有文件\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 32768;
    ofn.lpstrTitle = L"选择要发送到群的文件";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::string> paths;
    {
        std::wstring dir = buf;
        const wchar_t* p = buf + dir.size() + 1;
        if (*p == 0) {
            paths.push_back(nw::ws2u8(dir));
        } else {
            while (*p) {
                std::wstring fn = p;
                paths.push_back(nw::ws2u8(dir + L"\\" + fn));
                p += fn.size() + 1;
            }
        }
    }
    if (paths.empty()) return;
    std::wstring gname = gnameOf(currentGid_);
    wchar_t q[512];
    swprintf(q, 512, L"把所选 %d 个文件发送到群「%s」？\n群内所有成员将自动下载。", (int)paths.size(),
             gname.c_str());
    if (!askYesNo(this, L"发送文件", q, hwnd_)) return;
    std::string gid = currentGid_;
    nw::PeerCore* core = core_;
    nw::EventQueue* q2 = events_;
    std::thread([core, q2, paths, gid]() {
        for (auto& p : paths) {
            try {
                core->send_file_to_group(p, gid);
            } catch (const std::exception& e) {
                nw::Json args = nw::Json::arr();
                args.push(gid);
                args.push(std::string(nw::basename_utf8(p)) + " 发送失败：" + e.what());
                nw::Event ev("syschat", args);
                q2->push(ev);
            } catch (...) {
                nw::Json args = nw::Json::arr();
                args.push(gid);
                args.push(std::string("发送文件 ") + nw::basename_utf8(p) + " 失败");
                nw::Event ev("syschat", args);
                q2->push(ev);
            }
            Sleep(1000);
        }
    }).detach();
}

void App::sendChat() {
    std::wstring text = trimW(chatEntry_.text);
    if (text.empty()) return;
    if (currentGid_.empty()) {
        msgBox(this, L"提示", L"请先选择或创建一个群组", 0, hwnd_);
        return;
    }
    if (!core_->in_net()) {
        msgBox(this, L"提示", L"还没有加入内网，无法发送消息。", 0, hwnd_);
        switchNav("net");
        return;
    }
    core_->send_chat(currentGid_, nw::ws2u8(text));
    chatEntry_.clear();
    invalidate();
}

// ------------------------------------------------------------ 文件模型 --
void App::rebuildFileModel() {
    if (!core_) return;
    for (auto& g : core_->my_groups()) {
        std::string gid = g.get("gid").asStr("");
        auto& m = fileModel_[gid];
        for (auto& f : core_->files_of_group(gid)) {
            std::string fid = f.get("id").asStr("");
            if (m.find(fid) != m.end()) continue;
            FileRow r;
            r.fid = fid;
            r.name = U8(f.get("name").asStr(""));
            r.size = f.get("size").asInt(0);
            r.sender = U8(f.get("src_name").asStr(""));
            bool hasLocal = f.get("has_local").asBool(false);
            r.status = hasLocal ? L"已下载" : L"等待下载";
            r.progress = hasLocal ? 1.0 : 0.0;
            m[fid] = r;
        }
    }
    invalidate();
}

void App::onEvFileNew(const nw::Json& info) {
    std::string gid = info.get("gid").asStr("");
    nw::Json f = info.get("file");
    std::string fid = f.get("id").asStr("");
    auto& m = fileModel_[gid];
    if (m.find(fid) == m.end()) {
        FileRow r;
        r.fid = fid;
        r.name = U8(f.get("name").asStr(""));
        r.size = f.get("size").asInt(0);
        r.sender = U8(f.get("src_name").asStr(""));
        bool hasLocal = f.get("has_local").asBool(false);
        r.status = hasLocal ? L"已存在" : L"等待下载";
        r.progress = hasLocal ? 1.0 : 0.0;
        m[fid] = r;
    }
    if (gid == currentGid_ && chip_ == "files") renderFiles();
}

void App::onEvFileStatus(const nw::Json& st) {
    std::string fid = st.get("id").asStr("");
    std::string status = st.get("status").asStr("");
    for (auto& gm : fileModel_) {
        auto it = gm.second.find(fid);
        if (it == gm.second.end()) continue;
        FileRow& row = it->second;
        if (status == "downloading") {
            wchar_t buf[64];
            int pct = (int)(st.get("progress").asNum(0) * 100);
            swprintf(buf, 64, L"下载中 %d%%", pct);
            row.status = buf;
        } else if (status == "done") {
            row.status = L"已下载";
            row.path = U8(st.get("path").asStr(""));
            row.progress = 1.0;
            rebuildFileModel();
        } else if (status == "error") {
            row.status = L"失败：" + U8(st.get("note").asStr(""));
        }
        if (gm.first == currentGid_ && chip_ == "files") renderFiles();
    }
}

// ------------------------------------------------------------ 群消息 --
void App::onEvChat(const nw::Json& m) {
    std::string gid = m.get("gid").asStr("");
    ChatMsg cm;
    cm.sys = false;
    cm.from = U8(m.get("from_name").asStr(""));
    cm.from_uid = m.get("from_uid").asStr("");
    cm.text = U8(m.get("text").asStr(""));
    cm.ts = m.get("ts").asNum(0);
    cm.self = m.get("self").asBool(false);
    pushChatMsg(gid, cm);
    if (gid == currentGid_ && nav_ == "chat") {
        renderChat(true);
        scrollBottomLater();
    }
    if (!cm.self) {
        if (gid != currentGid_ || nav_ != "chat") {
            unread_[gid] = unread_.count(gid) ? unread_[gid] + 1 : 1;
        }
    }
    invalidate();
}

void App::onEvSyschat(const nw::Json& a) {
    if (a.size() < 2) return;
    std::string gid = a.at(0).asStr("");
    ChatMsg cm;
    cm.sys = true;
    cm.text = U8(a.at(1).asStr(""));
    cm.ts = nw::now_ts();
    pushChatMsg(gid, cm);
    if (gid == currentGid_ && nav_ == "chat") {
        renderChat(true);
        scrollBottomLater();
    }
    invalidate();
}

// ------------------------------------------------------------ 私聊事件 --
void App::onEvDm(const nw::Json& m) {
    bool self = m.get("self").asBool(false);
    std::string uid = self ? m.get("to_uid").asStr("") : m.get("from_uid").asStr("");
    if (uid.empty()) return;
    ChatWindow* w = findDm(uid);
    if (!w) {
        if (self) return;
        openDm(uid);
        w = findDm(uid);
        if (!w) return;
    }
    w->appendMsg(m);
    if (!self && !w->focused()) {
        w->markUnread();
        notify(L"私聊消息", L"「" + U8(m.get("from_name").asStr("?")) + L"」：" +
                                leftChars(U8(m.get("text").asStr("")), 60));
    }
}

void App::onEvDmSent(const nw::Json& info) {
    ChatWindow* w = findDm(info.get("to_uid").asStr(""));
    if (!w) return;
    if (info.get("ok").asBool(false)) {
        w->appendSys(L"对方不在线，已加密转存给在线同学「" +
                     U8(info.get("relay_name").asStr("?")) + L"」，待对方上线后自动送达");
        nw::Json m = nw::Json::obj();
        m.set("t", nw::Json("chat"));
        m.set("text", nw::Json(info.get("text").asStr("")));
        m.set("ts", nw::Json(info.get("ts").asNum(nw::now_ts())));
        m.set("self", nw::Json(true));
        w->appendMsg(m);
    } else {
        w->appendSys(U8(info.get("reason").asStr("转存失败")));
    }
}

// ------------------------------------------------------------ 改名投票 --
void App::onRenameRequest(const nw::Json& info) {
    if (info.get("from_uid").asStr("") == core_->uid) return;
    showRenameVoteDialog(this, info);
}

void App::onSettingsVoteProgress(const std::string& pid, int approve, int reject) {
    if (!settingsDlg_) return;
    SettingsForm* f = (SettingsForm*)settingsDlg_;
    f->onVoteProgress(pid, approve, reject);
}

void App::onSettingsVoteDone(const std::string& pid, bool ok, const std::wstring& reason,
                             const std::wstring& newName) {
    if (!settingsDlg_) return;
    SettingsForm* f = (SettingsForm*)settingsDlg_;
    f->onVoteDone(pid, ok, reason, newName);
}

void App::onSettingsClosed(void* p) {
    if (settingsDlg_ == p) settingsDlg_ = nullptr;
}

// ------------------------------------------------------------ 内网事件 --
void App::onNetChanged() {
    updateVoteBadge();
    if (nav_ == "net") renderNetPage();
    else if (nav_ == "vote") renderVotePage();
    else if (nav_ == "more") renderMorePage();
    refreshPeers();
    if (nav_ == "chat") invalidate();
}

void App::onNetVoteChanged() {
    updateVoteBadge();
    if (nav_ == "vote") renderVotePage();
    else if (nav_ == "net") renderNetPage();
    refreshVotePopup();
}

void App::onNetResult(bool ok, const std::wstring& text) {
    appendLog(nowStamp() + L" 内网: " + text);
    notify(L"内网", text);
    if (ok) msgBox(this, L"内网", text, 0, hwnd_);
    onNetChanged();
}

// ------------------------------------------------------------ 内网操作 --
void App::netScan() {
    netApplyLbl_ = L"正在扫描附近的内网…";
    invalidate();
    nw::PeerCore* core = core_;
    double win = core->cfg.get("net_scan_window").asNum(3.0);
    std::thread([core, win]() {
        try {
            core->net_scan(win);
        } catch (...) {
        }
    }).detach();
    post([this]() {
        if (nav_ == "net") renderNetPage();
    }, 1500);
}

void App::netCreate() {
    if (core_->in_net()) return;
    netCreateDialog(this);
}

void App::netRename() {
    if (!core_->in_net()) return;
    nw::Json v = core_->net_view();
    netRenameDialog(this, U8(v.get("name").asStr("")));
}

void App::netLeave() {
    if (!core_->in_net()) return;
    nw::Json v = core_->net_view();
    std::wstring q = L"确定退出内网「" + U8(v.get("name").asStr("")) +
                     L"」吗？\n退出后你会重新扫描附近内网（可能被要求重新投票加入）。";
    if (askYesNo(this, L"退出内网", q, hwnd_)) core_->net_leave();
}

void App::netApplySelected() {
    if (core_->in_net()) {
        msgBox(this, L"提示", L"你已经在内网里了", 0, hwnd_);
        return;
    }
    if (tNearby_.sel < 0 || tNearby_.sel >= (int)tNearby_.iids.size()) {
        msgBox(this, L"提示", L"请先在下面选择要加入的内网（可先「重新扫描」）", 0, hwnd_);
        return;
    }
    try {
        core_->net_apply_join(tNearby_.iids[(size_t)tNearby_.sel]);
    } catch (const std::exception& e) {
        msgBox(this, L"申请失败", U8(e.what()), 2, hwnd_);
    } catch (...) {
        msgBox(this, L"申请失败", L"未知错误", 2, hwnd_);
    }
}

void App::onNetCancelApply() { core_->net_cancel_apply(); }

void App::castVote(const std::string& req_id, int val) {
    try {
        core_->net_cast_vote(req_id, val);
    } catch (const std::exception& e) {
        msgBox(this, L"投票", U8(e.what()), 0, hwnd_);
        return;
    } catch (...) {
        msgBox(this, L"投票", L"投票失败", 0, hwnd_);
        return;
    }
    updateVoteBadge();
    if (nav_ == "vote") renderVotePage();
    else if (nav_ == "net") renderNetPage();
    refreshVotePopup();
}

// ------------------------------------------------------------ 退出群组 --
void App::leaveGroup(const std::string& gid) {
    auto it = core_->groups.find(gid);
    if (it == core_->groups.end()) return;
    std::wstring gname = U8(it->second.get("gname").asStr(""));
    std::wstring q = L"确定退出群「" + gname + L"」吗？\n（已下载的文件会保留在本机）";
    if (!askYesNo(this, L"退出群组", q, hwnd_)) return;
    core_->leave_group(gid);
    if (currentGid_ == gid) {
        currentGid_.clear();
        renderedN_ = 0;
        chatRowGid_.clear();
        chatRowN_ = 0;
        renderChat(false);
        renderFiles();
    }
    refreshGroupsList();
    refreshPeers();
}

void App::showGroupMenuAt(const std::string& gid, int x, int y) {
    showGroupMenu(this, gid, x, y);
}

void App::hideGroupMenu() { ui::hideGroupMenu(this); }

// ------------------------------------------------------------ 更新检查 --
void App::onManualUpdate() {
    if (manuallyCheck_) return;
    manuallyCheck_ = true;
    updateShown_ = false;              // 手动检查时允许再次弹窗
    core_->check_update(true);
}

void App::onEvUpdate(const nw::Json& res) {
    manuallyCheck_ = false;
    if (updateShown_) return;
    if (res.get("ok").asBool(false) && res.get("found").asBool(false)) {
        updateShown_ = true;
        showUpdateDialog(this, res);
    }
    // 未联网 / 版本号相同或更旧：静默
}

void App::startUpdate(const std::wstring& page) {
    if (updating_) return;
    std::string upd = nw::path_join(core_->exe_dir, nw::UPDATER1_PROGRAM_NAME);
    if (!nw::file_exists(upd)) {
        std::wstring msg = L"同目录下没有独立更新程序：\n" + U8(upd) + L"\n\n请把 " +
                           U8(nw::UPDATER1_PROGRAM_NAME) +
                           L" 与本程序放在同一个文件夹里，或直接运行「更新.bat」。";
        msgBox(this, L"找不到更新程序", msg, 1, hwnd_);
        if (!page.empty()) ShellExecuteW(nullptr, L"open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    std::wstring cmd = L"\"" + U8(upd) + L"\" --update";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cwd = U8(core_->exe_dir);
    if (!CreateProcessW(U8(upd).c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        cwd.c_str(), &si, &pi)) {
        wchar_t msg[256];
        swprintf(msg, 256, L"\u542f\u52a8\u66f4\u65b0\u7a0b\u5e8f\u5931\u8d25\uff1a\u9519\u8bef\u4ee3\u7801 %lu",
                 GetLastError());
        msgBox(this, L"启动更新程序失败", msg, 1, hwnd_);
        return;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    updating_ = true;
    appendLog(nowStamp() + L" 系统: 已启动独立更新程序 " + U8(nw::UPDATER1_PROGRAM_NAME) + L" --update");
    msgBox(this, L"已启动更新程序",
           L"已启动独立更新程序：\n" + U8(upd) +
               L"\n\n请在它弹出的窗口里完成更新（会先把你的会话内容备份到 customer_god/，\n"
               L"再把新版取回并接着用这些会话内容）。\n\n"
               L"更新完成后关闭本程序，到新版本目录双击「启动.bat」即可。",
           0, hwnd_);
}

// ------------------------------------------------------------ 空设备提示 --
void App::doBlankDevicePrompt() {
    if (blankPromptDone_) return;
    blankPromptDone_ = true;
    // 用「进程启动那一刻」的快照，避免 core.start() 后自动组网把判定冲掉
    if (!core_->was_blank_at_start()) return;
    showBlankDeviceDialog(this);
}

// ------------------------------------------------------------ 通知 --
void App::notify(const std::wstring& title, const std::wstring& msg) {
    if (notifier_.notify(title, msg)) return;
    showNotifyOverlay(title, msg);
}

void App::showNotifyOverlay(const std::wstring& title, const std::wstring& msg) {
    overlayOn_ = true;
    overlayTitle_ = title;
    overlayMsg_ = msg;
    if (overlayTimer_) cancelTimer(overlayTimer_);
    overlayTimer_ = post([this]() {
        overlayOn_ = false;
        overlayTimer_ = 0;
        invalidate();
    }, 6000);
    invalidate();
}

// ------------------------------------------------------- 投票悬浮面板 --
static const wchar_t* kPopupClass = L"NwVotePopup";
static bool g_popupClassReg = false;
static std::vector<Hot> g_popupHits;
static bool g_popupTracking = false;

static LRESULT CALLBACK PopupProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    App* app = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
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
            fillRect(g, Gdiplus::RectF(0, 0, (REAL)cr.right, (REAL)cr.bottom), C_BORDER);
            fillRect(g, Gdiplus::RectF(1, 1, (REAL)(cr.right - 2), (REAL)(cr.bottom - 2)),
                     C_PANEL);
            g_popupHits.clear();
            int x = 13;
            int y = 11;
            drawText(g, L"投票", fontPt(11, true), C_TEXT,
                     Gdiplus::RectF((REAL)x, (REAL)y, 60, (REAL)g_textH(11, true)), 0, 1, false,
                     false);
            drawText(g, L"鼠标移开自动收起", fontPt(8, false), C_SUB,
                     Gdiplus::RectF((REAL)(x + 44), (REAL)(y + 4), 160,
                                    (REAL)g_textH(8, false)),
                     0, 1, false, false);
            y += g_textH(11, true) + 4;
            std::vector<nw::Json> cards;
            if (app) cards = app->core()->net_pending_for_me();
            if (cards.empty()) {
                drawText(g, L"暂无待我投票的入网申请", fontPt(10, false), C_SUB,
                         Gdiplus::RectF((REAL)x, (REAL)(y + 10), 300, (REAL)g_textH(10, false)), 0,
                         1, false, false);
                y += 10 + g_textH(10, false) + 10;
            } else {
                int wCards = cr.right - 2 * x;
                for (auto& cj : cards) {
                    Card c = cardFromJson(cj);
                    int ch = cardHeight(measureG(), c, wCards, app ? app->core() : nullptr);
                    RECT rc{x, y, x + wCards, y + ch};
                    drawVoteCard(g, rc, c, g_popupHits, app ? app->core() : nullptr);
                    y += ch + 12;
                }
            }
            {
                std::wstring txt = L"打开投票页面";
                int tw = g_textW(txt, 9, false);
                int bx = cr.right - 12 - tw - 8;
                int bh = g_textH(9, false) + 6;
                drawText(g, txt, fontPt(9, false), C_ACCENT,
                         Gdiplus::RectF((REAL)bx, (REAL)y, (REAL)(tw + 8), (REAL)bh), 0, 1, false,
                         false);
                Hot h;
                h.rc = RECT{bx, y, bx + tw + 8, y + bh};
                h.act = A_VOTE_PAGE_OPEN;
                g_popupHits.push_back(h);
            }
            db.end();
            db.release();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_popupTracking) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            g_popupTracking = true;
        }
        if (app) app->cancelVoteHideJob();
        return 0;
    }
    case WM_MOUSELEAVE: {
        g_popupTracking = false;
        if (app) { app->scheduleVotePopupHide(); }
        return 0;
    }
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        for (auto& h : g_popupHits) {
            if (!ptIn(h.rc, x, y)) continue;
            if (h.act == A_VOTE_PAGE_OPEN) {
                if (app) {
                    app->hideVotePopup();
                    app->switchNav("vote");
                }
            } else if (h.act == A_VOTE_CAST) {
                if (app) app->castVote(h.arg, h.val);
            }
            break;
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

void App::scheduleVotePopupShow() {
    cancelVoteHideJob();
    if (popupShowTimer_) cancelTimer(popupShowTimer_);
    popupShowTimer_ = post([this]() {
        popupShowTimer_ = 0;
        showVotePopup();
    }, 200);
}

void App::scheduleVotePopupHide() {
    if (popupHideTimer_) cancelTimer(popupHideTimer_);
    popupHideTimer_ = post([this]() {
        popupHideTimer_ = 0;
        hideVotePopup();
    }, 450);
}

void App::cancelVoteHideJob() {
    if (popupHideTimer_) {
        cancelTimer(popupHideTimer_);
        popupHideTimer_ = 0;
    }
}

void App::showVotePopup() {
    if (!hwnd_) return;
    if (popup_) {
        refreshVotePopup();
        return;
    }
    if (!g_popupClassReg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PopupProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPopupClass;
        RegisterClassExW(&wc);
        g_popupClassReg = true;
    }
    // 先按内容估算宽高（绘制时会精确排版）
    int pw = 380;
    int ph = 240;
    {
        std::vector<nw::Json> cards = core_->net_pending_for_me();
        if (cards.empty()) ph = 40 + g_textH(11, true) + 4 + 10 + g_textH(10, false) + 10 +
                                   g_textH(9, false) + 16;
        else {
            int total = 40 + g_textH(11, true) + 4;
            for (auto& cj : cards) {
                Card c = cardFromJson(cj);
                total += cardHeight(measureG(), c, 380 - 26, core_) + 12;
            }
            total += g_textH(9, false) + 20;
            ph = total;
        }
    }
    int px = rcNav_[2].right + 6;
    int py = rcNav_[2].top - 12;
    POINT pt{px, py};
    ClientToScreen(hwnd_, &pt);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (pt.x + pw > sw - 20) pt.x = (std::max)(20, sw - pw - 20);
    if (pt.y + ph > sh - 40) pt.y = (std::max)(20, sh - ph - 60);
    popup_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPopupClass, L"", WS_POPUP, pt.x,
                             pt.y, pw, ph, hwnd_, nullptr, inst_, this);
    if (!popup_) return;
    ShowWindow(popup_, SW_SHOWNOACTIVATE);
    SetWindowPos(popup_, HWND_TOPMOST, pt.x, pt.y, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(popup_, 1, 300, nullptr);      // 内容随投票变化刷新
}

void App::refreshVotePopup() {
    if (popup_) InvalidateRect(popup_, nullptr, FALSE);
}

void App::hideVotePopup() {
    popupHideTimer_ = 0;
    if (popup_) {
        KillTimer(popup_, 1);
        DestroyWindow(popup_);
        popup_ = nullptr;
    }
    g_popupHits.clear();
    g_popupTracking = false;
}

} // namespace ui
