// test_interop.cpp —— 跨语言互通测试：C++ 版 ⇄ Python 版（neiwang.py）
//
// 验证目标：两版**协议完全兼容、可同网混用**。
//   1. presence / 群发现互通
//   2. 内网准入（C++ 建网 → Python 自动申请加入 → C++ 批准）
//   3. 群聊双向互通
//   4. 公共日志双向互通（并进入活动历史）
//   5. TCP cmd=act_get（活动历史补齐）互通
//   6. TCP cmd=get（文件下载）互通
//   7. TCP cmd=secret_store / cmd=secret（离线保密转存）互通 + 加密/HMAC 跨语言一致
//
// 依赖 tools/py_peer.py（无头 Python 对端，通过 --out 写状态 JSON 供本测试断言）。
#include "nw_test.h"
#include "../src/common/crypto.h"
#include "../src/common/sha2.h"

#include <windows.h>
#include <thread>
#include <chrono>

using namespace nw;
using namespace nwt;

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static std::wstring wq(const std::string& s) { return u82ws(s); }

static bool spawn_py(const std::string& root, const std::string& args, PROCESS_INFORMATION& pi) {
    std::string cmdline = "python \"" + root + "\\tools\\py_peer.py\" " + args;
    std::wstring wcmd = wq(cmdline);
    std::wstring wdir = wq(root);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = nul;
    si.hStdError = nul;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, wdir.c_str(), &si, &pi);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return ok == TRUE;
}

static bool read_state(const std::string& path, Json& out) {
    std::string text;
    if (!read_file_utf8(path, text) || text.empty()) return false;
    bool ok = false;
    Json j = Json::parse(text, &ok);
    if (!ok || !j.isObj()) return false;
    out = j;
    return true;
}

// 在状态文件的 events（[[kind, ...], ...]）里查找某 kind 且内容含 needle 的事件
static bool state_has_event(const Json& state, const std::string& kind, const std::string& needle) {
    Json evs = state.get("events");
    if (!evs.isArr()) return false;
    for (const Json& e : evs.items()) {
        if (!e.isArr() || e.items().empty()) continue;
        if (e.items()[0].asStr("") != kind) continue;
        if (needle.empty()) return true;
        if (e.dump().find(needle) != std::string::npos) return true;
    }
    return false;
}

int main() {
    net_init();
    set_env("NEIWANG_UDP_PORT", "46379");
    set_env("NEIWANG_TCP_PORT", "46381");
    set_env("NEIWANG_SCAN_WINDOW", "1");
    set_env("NEIWANG_NET_ANNOUNCE", "3");
    set_env("NEIWANG_NET_REQ_REBROADCAST", "2");
    set_env("NEIWANG_PRES_INTERVAL", "3");

    const std::string exeDir = get_exe_dir();
    const std::string root = exeDir + "\\..\\..";          // cpp\build -> 仓库根
    const std::string pyUid = "PPPPPPPPPPPP";
    const int pyTcp = 46382;
    std::string dirC = temp_dir("itC");
    std::string dirP = temp_dir("itP");
    std::string preload = path_join(dirP, "互通文件.txt");
    const std::string preloadContent = "跨语言文件互通测试内容 cross-language file interop";
    ensure_dir(dirP);
    write_file_utf8(preload, preloadContent);
    std::string statePath = path_join(dirP, "py_state.json");

    section("interop: 启动 C++ 核心并建立内网");
    EventQueue q;
    Json cfg = make_cfg(46379, 46381, "设备C", "CCCCCCCCCCCC");
    PeerCore C(cfg, &q, [] { return std::string("设备C"); }, dirC, dirC);
    C.start();
    for (int i = 0; i < 100 && !C.in_net(); ++i) sleep_ms(100);
    check(C.in_net(), "C++ 端已建立内网");
    std::string cnid = C.net.get("nid").asStr("");
    check(!cnid.empty(), "内网已有 nid");

    section("interop: 启动 Python 对端并自动申请加入内网");
    PROCESS_INFORMATION pi{};
    std::string args =
        "--data-dir \"" + dirP + "\" --name \"设备P\" --uid " + pyUid +
        " --udp 46379 --tcp " + std::to_string(pyTcp) +
        " --group G1:班群 --auto-join --act-hello"
        " --log \"Python 侧日志\" --chat \"G1:Python 侧群聊\""
        " --preload \"" + preload + "\""
        " --out \"" + statePath + "\" --seconds 45";
    bool spawned = spawn_py(root, args, pi);
    check(spawned, "已启动无头 Python 对端（tools/py_peer.py）");

    Json pyState;
    bool joined = false;
    for (int i = 0; i < 300; ++i) {           // 最多 30 秒
        sleep_ms(100);
        if (C.net_members.count(pyUid)) { joined = true; break; }
    }
    check(joined, "Python 对端通过入网申请被 C++ 端批准（内网准入互通）");

    section("interop: presence / 群发现");
    bool online = false;
    for (int i = 0; i < 60 && !online; ++i) {
        sleep_ms(100);
        online = C.is_online(pyUid);
    }
    check(online, "C++ 端看到 Python 对端在线（presence 互通）");
    if (C.peers.count(pyUid)) {
        check_eq(C.peers[pyUid].name, std::string("设备P"), "对方名字正确");
        check_eq(C.peers[pyUid].tcp_port, pyTcp, "对方 TCP 端口正确");
    }
    // 群发现要等 Python 端入网后加入 G1、并在下一次心跳里把 groups 广播出来
    bool discovered = false;
    for (int i = 0; i < 150 && !discovered; ++i) {
        sleep_ms(100);
        for (auto& kv : C.discovered) if (kv.first == "G1") discovered = true;
    }
    check(discovered, "C++ 端发现了 Python 端声明的群 G1");

    // 等 Python 端确认自己已加入 G1（状态文件里的 groups），再做群聊测试
    bool pyHasG1 = false;
    for (int i = 0; i < 150 && !pyHasG1; ++i) {
        sleep_ms(100);
        if (read_state(statePath, pyState)) {
            Json gs = pyState.get("groups");
            if (gs.isArr())
                for (const Json& g : gs.items())
                    if (g.get("gid").asStr("") == "G1") pyHasG1 = true;
        }
    }
    check(pyHasG1, "Python 端已加入群 G1");

    section("interop: 群聊双向");
    C.join_group("G1", "班群");
    check(C.groups.count("G1") > 0, "C++ 端加入群 G1");
    sleep_ms(600);
    C.send_chat("G1", "C++ 侧群聊");

    bool sawPyChat = false;
    for (int i = 0; i < 150 && !sawPyChat; ++i) {   // 最多 15 秒
        Event ev;
        while (q.pop(ev, 0)) {
            if (ev.kind == "chat" && ev.args.dump().find("Python 侧群聊") != std::string::npos)
                sawPyChat = true;
        }
        if (!sawPyChat) sleep_ms(100);
    }
    check(sawPyChat, "C++ 端收到 Python 端发来的群聊（Python → C++）");

    bool pySawChat = false;
    for (int i = 0; i < 150 && !pySawChat; ++i) {
        sleep_ms(100);
        if (read_state(statePath, pyState) && state_has_event(pyState, "chat", "C++ 侧群聊"))
            pySawChat = true;
    }
    check(pySawChat, "Python 端收到 C++ 端发来的群聊（C++ → Python）");

    section("interop: 公共日志双向");
    C.broadcast_log("C++ 侧日志");
    bool pySawLog = false;
    for (int i = 0; i < 100 && !pySawLog; ++i) {
        sleep_ms(100);
        if (read_state(statePath, pyState) && state_has_event(pyState, "log", "C++ 侧日志"))
            pySawLog = true;
    }
    check(pySawLog, "Python 端收到 C++ 端广播的公共日志");

    bool cppSawLog = false;
    for (int i = 0; i < 100 && !cppSawLog; ++i) {
        sleep_ms(100);
        for (auto& line : C.log_lines())
            if (line.find("Python 侧日志") != std::string::npos) cppSawLog = true;
        if (!cppSawLog) {
            for (auto& e : C.act_events)
                if (e.text.find("Python 侧日志") != std::string::npos) cppSawLog = true;
        }
    }
    check(cppSawLog, "C++ 端收到 Python 端广播的公共日志，并写入活动历史");

    section("interop: TCP cmd=act_get（活动历史补齐）");
    {
        std::vector<ActEvent> got;
        bool ok = C.act_fetch_from("127.0.0.1", pyTcp, 0.0, got);
        check(ok, "C++ 向 Python 请求活动历史成功");
        bool hasPy = false;
        for (auto& e : got) if (e.uid == pyUid) hasPy = true;
        check(hasPy, "取回的活动事件来自 Python 端（跨语言活动历史互通）");
    }

    section("interop: TCP cmd=get（文件下载）");
    {
        std::string preloadText;
        read_file_utf8(preload, preloadText);
        std::string fid = sha256_hex(preloadContent);
        DlTask task;
        task.fid = fid; task.name = "互通文件.txt"; task.size = (long long)preloadContent.size();
        task.gid = "G1"; task.gname = "班群";
        task.sender_uid = pyUid; task.sender_name = "设备P";
        check(C.fetch_from("127.0.0.1", pyTcp, fid, task), "C++ 从 Python 下载文件成功（cmd=get 互通）");
        // fetch_from 只落 cache/（正式入库由 finish_download 负责），因此直接校验缓存文件
        std::string cacheFile = path_join(C.cache_dir(), fid);
        check(file_exists(cacheFile), "文件已落入 cache/（SHA-256 校验通过）");
        std::string got;
        read_file_utf8(cacheFile, got);
        check_eq(got, preloadContent, "文件内容逐字节一致");
    }

    section("interop: TCP cmd=secret_store / cmd=secret（离线保密转存）");
    {
        PeerInfo p;
        p.uid = pyUid; p.ip = "127.0.0.1"; p.tcp_port = pyTcp; p.ts = now_ts();
        const std::string secretText = "跨语言保密转存：你好，世界 secret relay";
        const std::string key = random_bytes(32);
        const std::string storeId = "ITSTORE0001";
        Json rec = Json::obj();
        rec.set("to_uid", Json(pyUid));
        rec.set("from_uid", Json("CCCCCCCCCCCC"));
        rec.set("from_name", Json("设备C"));
        rec.set("blob", Json(secret_encrypt(secretText, key)));
        rec.set("key", Json(hex_encode(key)));
        rec.set("sig", Json(secret_sign(key, {pyUid, std::string("CCCCCCCCCCCC"), storeId, secretText})));
        rec.set("ts", Json(now_ts()));
        rec.set("retries", Json((long long)0));
        check(C.store_secret_on(p, storeId, rec), "Python 端接受寄存（cmd=secret_store 互通）");
        check(C.deliver_one(p, storeId, rec),
              "Python 端解密 + HMAC 校验通过并回执（cmd=secret 互通，跨语言加解密一致）");
    }

    section("interop: 活动历史（双方都写入了对方的数据）");
    {
        // 说明：record_activity 用「本机 uid」登记事件（与 Python 版一致），
        // 远端数据的来源体现在事件正文里（「名字: 文本」），所以按正文断言。
        bool cppHasPyText = false;
        for (auto& e : C.act_events)
            if (e.text.find("设备P") != std::string::npos) cppHasPyText = true;
        check(cppHasPyText, "C++ 活动历史里含 Python 端的记录（正文含「设备P」）");
        check(C.activity_view().get("count").asInt(0) > 0, "C++ 活动历史状态条有数据");

        bool pyHasCppText = false;
        if (read_state(statePath, pyState)) {
            Json lines = pyState.get("log_lines");
            if (lines.isArr())
                for (const Json& l : lines.items())
                    if (l.asStr("").find("设备C") != std::string::npos) pyHasCppText = true;
        }
        check(pyHasCppText, "Python 端日志里含 C++ 端的记录（正文含「设备C」）");
    }

    if (pi.hProcess) {
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    C.save_activity(true);
    C.stop();
    return summary("test_interop");
}
