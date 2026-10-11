// test_activity.cpp —— 新功能 A「局域网活动历史」测试
//   · 记录/持久化/合并去重
//   · TCP cmd=act_get 补齐（真实回环连接）
//   · act_hello → act_have → 补齐 的端到端（真实 UDP 广播 + TCP）
#include "nw_test.h"

#include <thread>
#include <chrono>

using namespace nw;
using namespace nwt;

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main() {
    net_init();
    set_env("NEIWANG_UDP_PORT", "45779");
    set_env("NEIWANG_TCP_PORT", "45731");
    set_env("NEIWANG_ACT_HELLO", "3600");
    set_env("NEIWANG_PRES_INTERVAL", "60");
    set_env("NEIWANG_NET_ANNOUNCE", "600");
    set_env("NEIWANG_NET_REQ_REBROADCAST", "600");

    // ---------------------------------------------------------- 1. 记录与持久化
    section("activity: 记录 / 事件ID / 持久化");
    {
        std::string dir = temp_dir("act1");
        EventQueue q;
        Json cfg = make_cfg(45779, 45731, "设备一", "AABBCCDDEEFF");
        PeerCore a(cfg, &q, [] { return std::string("设备一"); }, dir, dir);

        check(!a.has_activity_data(), "新设备无活动数据");
        a.record_activity("log", "设备一: 第一条");
        a.record_activity("log", "设备一: 第二条");
        check(a.has_activity_data(), "记录后有活动数据");
        check_eq((long long)a.activity_view().get("count").asInt(0), (long long)2, "活动条数 = 2");
        check_eq(a.activity_view().get("count").asInt(0), (long long)a.act_events.size(),
                 "activity_view.count 与 act_events 一致");
        check_eq(a.act_events[0].eid, std::string("AABBCCDDEEFF-1"), "首事件 eid = uid-1");
        check_eq(a.act_events[1].eid, std::string("AABBCCDDEEFF-2"), "次事件 eid = uid-2");
        check_eq(a.act_events[0].kind, std::string("log"), "kind 记录正确");
        a.save_activity(true);

        PeerCore a2(cfg, &q, [] { return std::string("设备一"); }, dir, dir);
        check_eq((long long)a2.act_events.size(), (long long)2, "重新加载后事件仍为 2 条");
        check_eq(a2.act_events[1].text, std::string("设备一: 第二条"), "重载文本一致");
    }

    // ---------------------------------------------------------- 2. 合并去重
    section("activity: merge_activity 去重 / 排序 / 序号");
    {
        std::string dir = temp_dir("act2");
        EventQueue q;
        Json cfg = make_cfg(45779, 45731, "设备一", "AABBCCDDEEFF");
        PeerCore a(cfg, &q, [] { return std::string("设备一"); }, dir, dir);
        a.record_activity("log", "本机事件");   // seq 1

        std::vector<ActEvent> incoming;
        ActEvent e1; e1.eid = "112233445566-1"; e1.seq = 1; e1.ts = 100.0;
        e1.uid = "112233445566"; e1.name = "设备二"; e1.kind = "log"; e1.text = "远程1";
        ActEvent e2; e2.eid = "112233445566-2"; e2.seq = 2; e2.ts = 90.0;
        e2.uid = "112233445566"; e2.name = "设备二"; e2.kind = "chat"; e2.text = "远程2";
        incoming.push_back(e1);
        incoming.push_back(e2);
        a.merge_activity(incoming);
        check_eq((long long)a.act_events.size(), (long long)3, "合并后 3 条");
        check_eq(a.act_events[0].eid, std::string("112233445566-2"), "按 ts 升序（90 在前）");
        check_eq(a.act_events[2].eid, std::string("AABBCCDDEEFF-1"), "本机事件 ts 最大排最后");
        check(a.act_last_get_ts > 0, "merge 后 last_get_ts 被更新");

        // 重复合并必须幂等
        a.merge_activity(incoming);
        check_eq((long long)a.act_events.size(), (long long)3, "重复合并不增加条数（eid 去重）");

        // 对方的 seq 不得污染本机 seq（否则 eid 冲突）
        a.record_activity("log", "再来一条");
        check_eq(a.act_events.back().eid, std::string("AABBCCDDEEFF-2"), "本机 seq 从 1 继续，不受远程 seq 影响");
    }

    // ---------------------------------------------------------- 3. TCP act_get
    section("activity: TCP cmd=act_get 补齐");
    {
        std::string dirB = temp_dir("actB");
        std::string dirA = temp_dir("actA");
        EventQueue qb, qa;
        Json cfgB = make_cfg(45779, 45732, "设备二", "112233445566");
        Json cfgA = make_cfg(45779, 45731, "设备一", "AABBCCDDEEFF");
        PeerCore b(cfgB, &qb, [] { return std::string("设备二"); }, dirB, dirB);
        PeerCore a(cfgA, &qa, [] { return std::string("设备一"); }, dirA, dirA);

        b.record_activity("log", "设备二: 22:00 之后的第一条");
        b.save_activity(true);
        double ts1 = b.act_events[0].ts;
        sleep_ms(20);
        b.record_activity("log", "设备二: 22:00 之后的第二条");
        b.save_activity(true);

        b.start();
        a.start();
        sleep_ms(200);
        int bport = b.my_tcp_port();
        check(bport > 0, "设备二 TCP 端口已绑定");

        std::vector<ActEvent> out;
        // 注意：提供方在回包后会自己记一条「已向…提供…」日志，所以必须在调用前取快照
        const long long bcount = (long long)b.act_events.size();
        check(bcount >= 2, "设备二至少有 2 条活动记录");
        bool ok = a.act_fetch_from("127.0.0.1", bport, 0.0, out);
        check(ok, "act_fetch_from 成功");
        check_eq((long long)out.size(), bcount, "since=0 取回设备二全部活动记录");

        long long expect_filtered = 0;
        for (auto& e : b.act_events)
            if (e.ts > ts1 + 0.001) expect_filtered++;
        std::vector<ActEvent> out2;
        bool ok2 = a.act_fetch_from("127.0.0.1", bport, ts1 + 0.001, out2);
        check(ok2, "按 since 过滤再次取数成功");
        check_eq((long long)out2.size(), expect_filtered, "只取回 ts 晚于 since 的记录");
        check(out2.size() >= 1 && out2[0].text == std::string("设备二: 22:00 之后的第二条"),
              "取回的第一条正是缺失的那条");

        // ---------------------------------------------------- 4. 端到端 UDP
        section("activity: act_hello → act_have → 补齐（真实 UDP 广播）");
        // 让 A 与 B 处于同一个内网（nid 相同）
        Json n = Json::obj();
        n.set("nid", Json("TESTNID1"));
        n.set("name", Json("测试内网"));
        n.set("founder_mac", Json("112233445566"));
        n.set("created_ts", Json(now_ts()));
        a.net = n;
        b.net = n;
        a.net_members.clear();
        b.net_members.clear();

        // A 先制造一次「较早的最后获取时间」，模拟 22:00 关机的设备
        a.record_activity("log", "设备一: 22:00 之前");
        a.save_activity(true);
        a.act_last_get_ts = now_ts() - 3600.0;   // 最后一次数据获取时间 = 1 小时前

        a.act_haves.clear();
        a.act_pulling = true;
        a.act_pull_deadline = now_ts() + 5.0;

        a.act_hello_broadcast();
        // 等 B 应答并回到 A
        for (int i = 0; i < 30 && a.act_haves.empty(); ++i) sleep_ms(100);
        check(!a.act_haves.empty(), "A 收到 B 的 act_have 应答（局域网活动校验数据）");
        check(a.act_haves.count("112233445566") > 0, "应答来自设备二（物理地址 112233445566）");

        long long before = (long long)a.act_events.size();
        a.act_try_pull();
        check(a.act_events.size() > (size_t)before, "补齐后 A 的活动事件增加");
        bool got_remote = false;
        for (auto& e : a.act_events)
            if (e.uid == "112233445566") got_remote = true;
        check(got_remote, "补齐的内容包含设备二的活动信息");
        a.save_activity(true);
        b.save_activity(true);
    }

    return summary("test_activity");
}
