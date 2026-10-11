// test_proxy.cpp —— 新功能 B「设备数据传输代理」测试
//   · 群内无人在线时才启动代理（在线请求 / 应答 / 寄存）
//   · 代理文件落盘 data/proxy/<pid>.proxy，UI 不可见
//   · proxy_consumed：holder 不是我就删除、是我则保留
//   · 历史合并时按「代理已接受 proxy_id=.. holder=..」删除代理
//   · 端到端：A 发起在线请求 → B 应答 → A 寄存 → B 拉取文件
#include "nw_test.h"
#include "../src/common/sha2.h"

#include <thread>
#include <chrono>

using namespace nw;
using namespace nwt;

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main() {
    net_init();
    set_env("NEIWANG_UDP_PORT", "45879");
    set_env("NEIWANG_TCP_PORT", "45831");
    set_env("NEIWANG_PROXY_WAIT", "1");
    set_env("NEIWANG_PRES_INTERVAL", "60");
    set_env("NEIWANG_NET_ANNOUNCE", "600");
    set_env("NEIWANG_PROXY_GROUP_WAIT", "0");
    set_env("NEIWANG_ACT_HELLO", "3600");

    // ------------------------------------------------------- 1. 直连寄存（file）
    section("proxy: TCP proxy_store 寄存文件代理");
    {
        std::string dirS = temp_dir("proxS");
        std::string dirH = temp_dir("proxH");
        EventQueue qs, qh;
        Json cfgS = make_cfg(45879, 45831, "设备一", "AABBCCDDEEFF");
        Json cfgH = make_cfg(45879, 45832, "设备二", "112233445566");
        PeerCore S(cfgS, &qs, [] { return std::string("设备一"); }, dirS, dirS);
        PeerCore H(cfgH, &qh, [] { return std::string("设备二"); }, dirH, dirH);

        const std::string content = "作业内容：请查收 —— proxy 测试";
        const std::string fid = sha256_hex(content);
        ensure_dir(path_join(dirS, "cache"));
        write_file_utf8(path_join(path_join(dirS, "cache"), fid), content);

        S.start();
        H.start();
        sleep_ms(150);

        // 让 H 知道怎么找到 S（真实流程里来自 presence）
        PeerInfo si;
        si.uid = "AABBCCDDEEFF"; si.name = "设备一"; si.ip = "127.0.0.1";
        si.tcp_port = S.my_tcp_port();
        H.peers[si.uid] = si;

        SOCKET c = tcp_connect("127.0.0.1", H.my_tcp_port(), 3000);
        check(c != INVALID_SOCKET, "连接到设备二的 TCP 端口");
        if (c != INVALID_SOCKET) {
            Json req = Json::obj();
            req.set("cmd", Json("proxy_store"));
            req.set("proxy_id", Json("p_test_0001"));
            req.set("kind", Json("file"));
            req.set("gid", Json("G1"));
            req.set("gname", Json("班群"));
            req.set("file_id", Json(fid));
            req.set("name", Json("作业.txt"));
            req.set("size", Json((long long)content.size()));
            req.set("text", Json(""));
            req.set("sender_uid", Json("AABBCCDDEEFF"));
            req.set("sender_name", Json("设备一"));
            req.set("targets", Json::arr());
            req.set("ip", Json("127.0.0.1"));
            req.set("port", Json((long long)S.my_tcp_port()));
            req.set("ts", Json(now_ts()));
            send_json(c, req);
            Json resp;
            bool got = recv_json(c, resp);
            close_socket(c);
            check(got, "收到代理寄存应答");
            check_eq(resp.get("cmd").asStr(""), std::string("proxy_stored"), "应答 cmd=proxy_stored");
            check(resp.get("ok").asBool(false), "寄存成功");
        }

        check_eq((long long)H.proxy_list().size(), (long long)1, "设备二持有 1 条代理");
        std::string pfile = path_join(path_join(dirH, "proxy"), "p_test_0001.proxy");
        check(file_exists(pfile), "代理文件已落盘 data/proxy/<pid>.proxy（后缀自动改为 .proxy）");
        std::string got;
        read_file_utf8(pfile, got);
        check_eq(got, content, "代理文件内容与原文一致");
        check(H.has_file(fid) == false, "代理内容不进入正常文件缓存/界面列表");

        // ---------------------------------------------- 2. consumed 语义
        section("proxy: proxy_consumed 删除规则");
        H.on_proxy_consumed([] {
            Json m = Json::obj();
            m.set("proxy_id", Json("p_test_0001"));
            m.set("holder", Json("CCCCCCCCCCCC"));   // 新持有者不是我 -> 删除
            m.set("mac", Json("CCCCCCCCCCCC"));
            return m;
        }());
        check_eq((long long)H.proxy_list().size(), (long long)0, "holder 非本机 → 删除本地代理");
        check(!file_exists(pfile), "代理文件同时被删除");

        // 再存一次，测试 holder == 本机 时保留
        SOCKET c2 = tcp_connect("127.0.0.1", H.my_tcp_port(), 3000);
        if (c2 != INVALID_SOCKET) {
            Json req = Json::obj();
            req.set("cmd", Json("proxy_store"));
            req.set("proxy_id", Json("p_test_0002"));
            req.set("kind", Json("file"));
            req.set("gid", Json("G1"));
            req.set("gname", Json("班群"));
            req.set("file_id", Json(fid));
            req.set("name", Json("作业.txt"));
            req.set("size", Json((long long)content.size()));
            req.set("sender_uid", Json("AABBCCDDEEFF"));
            req.set("sender_name", Json("设备一"));
            req.set("targets", Json::arr());
            req.set("ip", Json("127.0.0.1"));
            req.set("port", Json((long long)S.my_tcp_port()));
            req.set("ts", Json(now_ts()));
            send_json(c2, req);
            Json resp;
            recv_json(c2, resp);
            close_socket(c2);
        }
        check_eq((long long)H.proxy_list().size(), (long long)1, "重新寄存成功");
        {
            Json m = Json::obj();
            m.set("proxy_id", Json("p_test_0002"));
            m.set("holder", Json("112233445566"));    // 新持有者就是我 -> 保留
            m.set("mac", Json("112233445566"));
            H.on_proxy_consumed(m);
        }
        check_eq((long long)H.proxy_list().size(), (long long)1, "holder 是本机 → 保留代理（成为分发源）");
        check(H.proxy_delete("p_test_0002", "112233445566") == false, "proxy_delete 幂等：持有者是自己时不删");
        check(H.proxy_delete("p_test_0002", "") == true, "proxy_delete 幂等：holder 为空时删除");
        check(H.proxy_delete("p_test_0002", "") == false, "proxy_delete 再次调用安全（已不存在）");

        // ---------------------------------------------- 3. 历史合并触发删除
        section("proxy: 从活动历史合并中执行删除（离线错过 → 补齐后仍正确）");
        {
            // 先寄存一条信息代理
            SOCKET c3 = tcp_connect("127.0.0.1", H.my_tcp_port(), 3000);
            if (c3 != INVALID_SOCKET) {
                Json req = Json::obj();
                req.set("cmd", Json("proxy_store"));
                req.set("proxy_id", Json("p_msg_0001"));
                req.set("kind", Json("msg"));
                req.set("gid", Json("G1"));
                req.set("gname", Json("班群"));
                req.set("text", Json("今晚作业在第 3 页"));
                req.set("sender_uid", Json("AABBCCDDEEFF"));
                req.set("sender_name", Json("设备一"));
                send_json(c3, req);
                Json resp;
                recv_json(c3, resp);
                close_socket(c3);
            }
            check_eq((long long)H.proxy_list().size(), (long long)1, "信息代理已寄存");

            std::vector<ActEvent> ev;
            ActEvent e;
            e.eid = "DDDDDDDDDDDD-1";
            e.seq = 1;
            e.ts = now_ts();
            e.uid = "DDDDDDDDDDDD";
            e.name = "设备四";
            e.kind = "proxy";
            e.text = "代理已接受 proxy_id=p_msg_0001 holder=";
            ev.push_back(e);
            H.merge_activity(ev);
            check_eq((long long)H.proxy_list().size(), (long long)0,
                     "从历史补齐「代理已接受」后自动删除本地代理");
            check(!file_exists(path_join(path_join(dirH, "proxy"), "p_msg_0001.proxy")),
                  "信息代理记录被删除");
        }

        // ---------------------------------------------- 4. 端到端（真实 UDP+TCP）
        section("proxy: 端到端 在线请求 → 应答 → 寄存 → 拉取");
        std::string dirA = temp_dir("proxA");
        std::string dirB = temp_dir("proxB");
        EventQueue qa, qb;
        Json cfgA = make_cfg(45879, 45833, "设备一", "AAAAAAAAAAAA");
        Json cfgB = make_cfg(45879, 45834, "设备二", "BBBBBBBBBBBB");
        // A 有群 G1；B 不在群里（是局域网内其他在线成员）
        Json groupsA = Json::arr();
        Json g1 = Json::obj(); g1.set("gid", Json("G1")); g1.set("gname", Json("班群"));
        groupsA.push(g1);
        cfgA.set("groups", groupsA);

        PeerCore A(cfgA, &qa, [] { return std::string("设备一"); }, dirA, dirA);
        PeerCore B(cfgB, &qb, [] { return std::string("设备二"); }, dirB, dirB);
        A.groups["G1"] = g1;
        B.groups.clear();

        const std::string content2 = "代理端到端测试文件";
        const std::string fid2 = sha256_hex(content2);
        ensure_dir(path_join(dirA, "cache"));
        write_file_utf8(path_join(path_join(dirA, "cache"), fid2), content2);

        // 两者同处一个内网
        Json n = Json::obj();
        n.set("nid", Json("PROXYNID"));
        n.set("name", Json("测试内网"));
        n.set("founder_mac", Json("AAAAAAAAAAAA"));
        n.set("created_ts", Json(now_ts()));
        A.net = n;
        B.net = n;
        A.net_members.clear();
        B.net_members.clear();
        // 内网成员登记表（真实流程由 net_registry 同步）：member_ok 需要它
        auto mk_member = [](const char* mac, const char* nm) {
            Json m = Json::obj();
            m.set("mac", Json(mac));
            m.set("name", Json(nm));
            m.set("ip", Json("127.0.0.1"));
            m.set("tcp_port", Json((long long)0));
            m.set("joined_ts", Json(now_ts()));
            m.set("founder", Json(false));
            return m;
        };
        A.net_members["AAAAAAAAAAAA"] = mk_member("AAAAAAAAAAAA", "设备一");
        A.net_members["BBBBBBBBBBBB"] = mk_member("BBBBBBBBBBBB", "设备二");
        B.net_members["AAAAAAAAAAAA"] = mk_member("AAAAAAAAAAAA", "设备一");
        B.net_members["BBBBBBBBBBBB"] = mk_member("BBBBBBBBBBBB", "设备二");

        A.start();
        B.start();
        sleep_ms(200);

        // A 眼里的 B：局域网内在线，但不在群里
        PeerInfo bp;
        bp.uid = "BBBBBBBBBBBB"; bp.name = "设备二"; bp.ip = "127.0.0.1";
        bp.tcp_port = B.my_tcp_port(); bp.ts = now_ts();
        bp.groups = Json::arr();
        A.peers[bp.uid] = bp;
        // B 眼里的 A
        PeerInfo ap;
        ap.uid = "AAAAAAAAAAAA"; ap.name = "设备一"; ap.ip = "127.0.0.1";
        ap.tcp_port = A.my_tcp_port(); ap.ts = now_ts();
        ap.groups = groupsA;
        B.peers[ap.uid] = ap;

        Json fileInfo = Json::obj();
        fileInfo.set("id", Json(fid2));
        fileInfo.set("name", Json("端到端.txt"));
        fileInfo.set("size", Json((long long)content2.size()));

        bool started = A.proxy_start("file", "G1", fileInfo, "");
        check(started, "群内无其他成员在线 → 启动设备数据传输代理");

        for (int i = 0; i < 30 && B.proxy_list().size() == 0; ++i) sleep_ms(100);
        check(B.proxy_list().size() >= 1, "设备二已寄存设备一的文件代理");
        if (B.proxy_list().size() >= 1) {
            std::string pid = B.proxy_list().at(0).get("proxy_id").asStr("");
            std::string pf = path_join(path_join(dirB, "proxy"), pid + ".proxy");
            check(file_exists(pf), "代理文件已拉取到设备二");
            std::string c;
            read_file_utf8(pf, c);
            check_eq(c, content2, "拉取的代理文件内容正确（SHA-256 校验通过）");
        }

        A.save_activity(true);
        B.save_activity(true);
        S.save_activity(true);
        H.save_activity(true);
    }

    return summary("test_proxy");
}
