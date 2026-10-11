// peercore_activity.cpp —— 局域网活动历史与补齐（design_v2 第 1 节）
//
// 本文件只实现 PeerCore 的活动历史成员（core.h 的 "peercore_activity.cpp" 段），
// 不修改任何其它文件。协议（UDP act_hello / act_have，TCP act_get / act_data）
// 与 data/active_log.json 的字段与 Python 版逐字一致。
#include "core.h"

#include "../common/sha2.h"
#include "../common/util.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace nw {

namespace {

// ------------------------------------------------------------------ 缓冲 --
const Cfg& act_cfg() {
    static const Cfg cache = [] {
        Cfg c;
        load_env_overrides(c);
        return c;
    }();
    return cache;
}

double act_min_ts(const std::vector<ActEvent>& ev) {
    bool first = true;
    double m = 0.0;
    for (const ActEvent& e : ev) {
        if (first || e.ts < m) { m = e.ts; first = false; }
    }
    return first ? 0.0 : m;
}

void act_truncate(std::vector<ActEvent>& ev, double& first_ts, int max_events) {
    if (max_events <= 0) max_events = 1;
    while ((long long)ev.size() > (long long)max_events) ev.erase(ev.begin());
    first_ts = ev.empty() ? 0.0 : act_min_ts(ev);
}

std::string act_jstr(const Json& j, const char* k) {
    if (!j.isObj()) return std::string();
    return j.get(k).asStr("");
}

double act_jnum(const Json& j, const char* k) {
    if (!j.isObj()) return 0.0;
    return j.get(k).asNum(0);
}

long long act_jint(const Json& j, const char* k) {
    if (!j.isObj()) return 0;
    return j.get(k).asInt(0);
}

// 固定 3 位小数：给校验指纹用，保证跨设备/跨语言字符串完全一致。
std::string act_f3(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}

// 显示行 = "[时间] 正文"
std::string act_line(double ts, const std::string& body) {
    return "[" + fmt_time(ts) + "] " + body;
}

// act_have 应答节流：core.h 没有 act_last_have 成员，这里按实例记录最后一次应答时间。
// 受独立互斥量保护（只在持有 mtx 时访问，锁序固定：mtx -> g_act_have_mu）。
std::mutex g_act_have_mu;
std::map<const PeerCore*, double> g_act_have_last;

} // namespace

// ================================================================ 持久化 ==

void PeerCore::load_activity() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (act_run_since <= 0) act_run_since = now_ts();

    act_events.clear();
    act_seq = 0;
    act_first_ts = 0.0;
    act_last_get_ts = 0.0;
    act_net_id.clear();

    const std::string path = path_join(data_root, "active_log.json");
    std::string text;
    if (!file_exists(path) || !read_file_utf8(path, text) || text.empty()) return;

    bool ok = false;
    Json d = Json::parse(text, &ok);
    if (!ok || !d.isObj()) return;   // 文件无法解析 = 本机没有任何活动数据

    act_net_id = d.get("net_id").asStr("");
    act_last_get_ts = d.get("last_get_ts").asNum(0);
    act_first_ts = d.get("first_ts").asNum(0);
    act_seq = d.get("seq").asInt(0);

    Json ev = d.get("events");
    if (ev.isArr()) {
        for (const Json& x : ev.items()) {
            if (!x.isObj()) continue;
            ActEvent e = act_event_from_json(x);
            act_events.push_back(e);
            if (e.seq > act_seq) act_seq = e.seq;
        }
    }
    act_first_ts = act_min_ts(act_events);
    act_truncate(act_events, act_first_ts, act_cfg().act_max_events);
}

void PeerCore::save_activity(bool force) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    const double now = now_ts();
    if (!force && now - act_last_save < 0.5) return;   // 节流：最多每 0.5s 一次
    act_last_save = now;

    Json o = Json::obj();
    o.set("net_id", Json(act_net_id));
    o.set("my_mac", Json(uid));
    o.set("last_get_ts", Json(act_last_get_ts));
    o.set("first_ts", Json(act_first_ts));
    o.set("seq", Json((long long)act_seq));

    Json arr = Json::arr();
    for (const ActEvent& e : act_events) arr.push(act_event_to_json(e));
    o.set("events", arr);

    ensure_dir(data_root);
    write_file_utf8(path_join(data_root, "active_log.json"), o.dump(true));
}

// ============================================================== 序列化 ==

Json PeerCore::act_event_to_json(const ActEvent& e) {
    Json j = Json::obj();
    j.set("eid", Json(e.eid));
    j.set("seq", Json((long long)e.seq));
    j.set("ts", Json(e.ts));
    j.set("uid", Json(e.uid));
    j.set("name", Json(e.name));
    j.set("kind", Json(e.kind));
    j.set("text", Json(e.text));
    return j;
}

ActEvent PeerCore::act_event_from_json(const Json& j) {
    ActEvent e;
    if (!j.isObj()) return e;
    e.eid = j.get("eid").asStr("");
    e.seq = j.get("seq").asInt(0);
    e.ts = j.get("ts").asNum(0);
    e.uid = j.get("uid").asStr("");
    e.name = j.get("name").asStr("");
    e.kind = j.get("kind").asStr("log");
    e.text = j.get("text").asStr("");
    if (e.eid.empty() && !e.uid.empty() && e.seq > 0)
        e.eid = e.uid + "-" + std::to_string(e.seq);
    return e;
}

// ============================================================== 查询 ==

bool PeerCore::has_activity_data() const {
    PeerCore* self = const_cast<PeerCore*>(this);
    std::lock_guard<std::recursive_mutex> lk(self->mtx);
    return !act_events.empty();
}

// core.h 已把访问器改名为 activity_last_get_ts()，与数据成员 act_last_get_ts 不再冲突。
double PeerCore::activity_last_get_ts() const {
    PeerCore* self = const_cast<PeerCore*>(this);
    std::lock_guard<std::recursive_mutex> lk(self->mtx);
    return act_last_get_ts;
}

Json PeerCore::activity_view() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    Json o = Json::obj();
    o.set("count", Json((long long)act_events.size()));
    o.set("last_get_ts", Json(act_last_get_ts));
    o.set("first_ts", Json(act_first_ts));
    return o;
}

// ============================================================== 记录 ==

void PeerCore::record_activity(const std::string& kind, const std::string& text, double ts) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (act_run_since <= 0) act_run_since = now_ts();
    if (ts <= 0) ts = now_ts();

    ActEvent e;
    e.seq = ++act_seq;
    e.eid = uid + "-" + std::to_string(e.seq);
    e.ts = ts;
    e.uid = uid;
    e.name = my_name();
    e.kind = kind;
    e.text = text;

    act_events.push_back(e);
    if (act_first_ts <= 0) act_first_ts = ts;
    act_truncate(act_events, act_first_ts, act_cfg().act_max_events);
    save_activity(false);
}

void PeerCore::merge_activity(const std::vector<ActEvent>& events) {
    std::lock_guard<std::recursive_mutex> lk(mtx);

    std::set<std::string> known;
    for (const ActEvent& e : act_events) {
        if (!e.eid.empty()) known.insert(e.eid);
    }

    std::vector<ActEvent> added;
    for (const ActEvent& e : events) {
        if (e.eid.empty() || known.count(e.eid)) continue;   // 按 eid 去重
        known.insert(e.eid);
        // 代理已被接受：活动历史里补齐这条记录时，同步执行同样的删除动作
        if (e.kind == "proxy" && e.text.rfind("代理已接受 proxy_id=", 0) == 0) {
            act_apply_consumed_text(e.text);
        }
        act_events.push_back(e);
        added.push_back(e);
    }

    std::sort(act_events.begin(), act_events.end(),
              [](const ActEvent& a, const ActEvent& b) {
                  if (a.ts != b.ts) return a.ts < b.ts;
                  return a.eid < b.eid;
              });

    // seq 只取本机已有的最大序号（不采用对方的 seq，避免序号冲突）
    long long local_max = act_seq;
    for (const ActEvent& e : act_events) {
        if (e.uid == uid && e.seq > local_max) local_max = e.seq;
    }
    act_seq = local_max;

    act_first_ts = act_min_ts(act_events);
    act_last_get_ts = now_ts();
    act_truncate(act_events, act_first_ts, act_cfg().act_max_events);
    save_activity(true);

    if (!added.empty()) {
        Json arr = Json::arr();
        for (const ActEvent& e : added) {
            arr.push(Json("[" + fmt_time(e.ts) + "] " + e.name + ": " + e.text));
        }
        emit("act_log", {arr});
    }
}

void PeerCore::act_apply_consumed_text(const std::string& text) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    auto field = [&text](const char* key) -> std::string {
        std::string pat = std::string(key) + "=";
        size_t p = text.find(pat);
        if (p == std::string::npos) return std::string();
        p += pat.size();
        size_t e = text.find(' ', p);
        std::string v = (e == std::string::npos) ? text.substr(p) : text.substr(p, e - p);
        return trim(v);
    };
    const std::string pid = field("proxy_id");
    if (pid.empty()) return;
    const std::string holder = field("holder");
    proxy_delete(pid, holder);
}

// ============================================================== 广播 ==

void PeerCore::act_hello_broadcast() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    // net_id 用「当前所在内网」，未入网时为空串（design_v2 1.3(1)）
    const std::string nid = in_net() ? net.get("nid").asStr("") : std::string();
    if (!nid.empty()) act_net_id = nid;   // 同步持久化字段（active_log.json 的 net_id）

    Json m = Json::obj();
    m.set("t", Json("act_hello"));
    m.set("mac", Json(uid));
    m.set("ip", Json(local_ipv4()));
    m.set("port", Json((long long)my_tcp_port()));
    m.set("last_get_ts", Json(act_last_get_ts));
    m.set("first_ts", Json(act_first_ts));
    m.set("count", Json((long long)act_events.size()));
    m.set("run_since", Json(act_run_since));
    m.set("net_id", Json(nid));
    m.set("ts", Json(now_ts()));
    broadcast(m);
}

void PeerCore::on_act_hello(Json msg, const std::string& from_ip) {
    (void)from_ip;
    std::lock_guard<std::recursive_mutex> lk(mtx);

    if (!in_net()) return;                                   // 未入网（或对方未入网）不应答
    const std::string my_nid = net.isObj() ? net.get("nid").asStr("") : std::string();
    if (my_nid.empty()) return;
    if (act_jstr(msg, "net_id") != my_nid) return;           // 不是本内网
    if (act_jstr(msg, "mac") == uid) return;                 // 自己的广播
    if (act_events.empty()) return;                          // 本机无数据：不应答

    const double now = now_ts();
    {
        std::lock_guard<std::mutex> g(g_act_have_mu);
        double& last = g_act_have_last[this];
        if (now - last < act_cfg().act_have_throttle) return;
        last = now;
    }

    double max_ts = 0.0;
    for (const ActEvent& e : act_events) {
        if (e.ts > max_ts) max_ts = e.ts;
    }

    Json r = Json::obj();
    r.set("t", Json("act_have"));
    r.set("mac", Json(uid));
    r.set("ip", Json(local_ipv4()));
    r.set("port", Json((long long)my_tcp_port()));
    r.set("last_get_ts", Json(act_last_get_ts));
    r.set("first_ts", Json(act_first_ts));
    r.set("count", Json((long long)act_events.size()));
    r.set("run_since", Json(act_run_since));
    r.set("max_ts", Json(max_ts));

    // sig = sha256(mac|count|last_get_ts|max_ts) 的前 16 个 hex 字符
    // 浮点统一 "%.3f"，保证跨设备、跨语言（Python）字符串完全一致。
    const std::string src = uid + "|" + std::to_string((long long)act_events.size()) + "|" +
                            act_f3(act_last_get_ts) + "|" + act_f3(max_ts);
    std::string sig = sha256_hex(src);
    if (sig.size() > 16) sig = sig.substr(0, 16);
    r.set("sig", Json(sig));

    r.set("net_id", Json(my_nid));
    r.set("ts", Json(now));
    broadcast(r);
}

void PeerCore::on_act_have(Json msg) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!act_pulling) return;
    const double now = now_ts();
    if (now >= act_pull_deadline) return;

    const std::string mac = act_jstr(msg, "mac");
    if (mac.empty() || mac == uid) return;

    if (act_haves.find(mac) == act_haves.end()) {
        Json m = msg;
        m.set("_arrive", Json((long long)act_haves.size()));   // 到达顺序（仅本地使用，不上线）
        act_haves[mac] = m;
    }

    const std::string name = act_jstr(msg, "name");
    const double run_since = act_jnum(msg, "run_since");
    const double last_get = act_jnum(msg, "last_get_ts");
    const long long count = act_jint(msg, "count");

    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "收到「%s」（物理地址 %s）的局域网活动校验数据：连续运行自 %s，最后数据 %s，共 %lld 条",
                  name.empty() ? "?" : name.c_str(), fmt_mac(mac).c_str(),
                  fmt_time(run_since).c_str(), fmt_time(last_get).c_str(), count);
    const std::string body(buf);
    emit("log", {Json(act_line(now, body))});
    record_activity("log", body);
}

// ============================================================== 补齐 ==

bool PeerCore::act_fetch_from(const std::string& ip, int port, double since,
                              std::vector<ActEvent>& out) {
    out.clear();
    if (ip.empty() || port <= 0) return false;

    SOCKET s = tcp_connect(ip, port, 8000);
    if (s == INVALID_SOCKET) return false;

    Json req = Json::obj();
    req.set("cmd", Json("act_get"));
    req.set("since", Json(since));
    req.set("mac", Json(uid));
    req.set("name", Json(my_name()));

    bool ok = false;
    if (send_json(s, req)) {
        Json resp;
        if (recv_json(s, resp) && resp.isObj() && resp.get("cmd").asStr("") == "act_data") {
            Json ev = resp.get("events");
            if (ev.isArr()) {
                for (const Json& x : ev.items()) {
                    if (!x.isObj()) continue;
                    out.push_back(act_event_from_json(x));
                }
            }
            ok = true;   // {"cmd":"err",...} 会落到这里返回 false
        }
    }
    close_socket(s);
    return ok;
}

void PeerCore::act_try_pull() {
    std::string ip, name, sel_mac = "";
    int port = 0;
    double my_last = 0.0;
    bool have_own = false;

    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        act_pulling = false;
        my_last = act_last_get_ts;
        have_own = !act_events.empty();

        const Json* best = nullptr;
        long long best_arrive = LLONG_MAX;
        // 合格应答：run_since <= my_last + 1（对方能覆盖我的空档），取最先到达的
        for (const auto& kv : act_haves) {
            if (act_jnum(kv.second, "run_since") <= my_last + 1.0) {
                const long long arrive = act_jint(kv.second, "_arrive");
                if (arrive < best_arrive) {
                    best_arrive = arrive;
                    best = &kv.second;
                    sel_mac = kv.first;
                }
            }
        }
        // 退化：count 更多者优先，其次 max_ts 更大者
        if (!best) {
            long long best_count = LLONG_MIN;
            double best_max = -1.0;
            for (const auto& kv : act_haves) {
                const long long c = act_jint(kv.second, "count");
                const double mx = act_jnum(kv.second, "max_ts");
                if (!best || c > best_count || (c == best_count && mx > best_max)) {
                    best = &kv.second;
                    best_count = c;
                    best_max = mx;
                    sel_mac = kv.first;
                }
            }
        }

        if (!best) {
            if (have_own) {
                const double now = now_ts();
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                              "局域网内暂无可用于补齐活动数据的设备（本机最后数据时间 %s）",
                              fmt_time(my_last).c_str());
                const std::string body(buf);
                emit("log", {Json(act_line(now, body))});
                record_activity("log", body);
            }
            act_haves.clear();
            return;
        }

        name = act_jstr(*best, "name");
        ip = act_jstr(*best, "ip");
        port = (int)act_jint(*best, "port");

        {
            const double now = now_ts();
            char buf[384];
            std::snprintf(buf, sizeof(buf),
                          "已选定响应最快的「%s」（物理地址 %s）构建连接补齐本机活动数据",
                          name.empty() ? "?" : name.c_str(), fmt_mac(sel_mac).c_str());
            const std::string body(buf);
            emit("log", {Json(act_line(now, body))});
            record_activity("log", body);
        }
        act_haves.clear();   // 窗口结束，不再接受新应答
    }

    // 网络 I/O 在锁外进行（tcp_connect 超时 8s）
    std::vector<ActEvent> events;
    const bool ok = act_fetch_from(ip, port, my_last, events);

    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (ok) {
        const double now = now_ts();
        const long long n = (long long)events.size();
        merge_activity(events);
        char buf[384];
        std::snprintf(buf, sizeof(buf),
                      "已从「%s」补齐 %s 至 %s 的局域网活动信息（共 %lld 条）",
                      name.empty() ? "?" : name.c_str(),
                      fmt_time(my_last).c_str(), fmt_time(now).c_str(), n);
        const std::string body(buf);
        emit("log", {Json(act_line(now, body))});
        record_activity("log", body);
    } else if (have_own) {
        const double now = now_ts();
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "局域网内暂无可用于补齐活动数据的设备（本机最后数据时间 %s）",
                      fmt_time(my_last).c_str());
        const std::string body(buf);
        emit("log", {Json(act_line(now, body))});
        record_activity("log", body);
    }
}

// ============================================================== 应答 ==

void PeerCore::act_on_get(Json req, SOCKET conn) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    const double since = act_jnum(req, "since");
    const std::string rname = act_jstr(req, "name").empty() ? std::string("?") : act_jstr(req, "name");
    const int limit = act_cfg().act_send_limit;

    Json arr = Json::arr();
    long long n = 0;
    for (const ActEvent& e : act_events) {          // 从最早开始，最多 act_send_limit 条
        if (e.ts > since) {
            arr.push(act_event_to_json(e));
            if (++n >= (long long)limit) break;
        }
    }

    Json resp = Json::obj();
    resp.set("cmd", Json("act_data"));
    resp.set("ok", Json(true));
    resp.set("mac", Json(uid));
    resp.set("name", Json(my_name()));
    resp.set("first_ts", Json(act_first_ts));
    resp.set("last_get_ts", Json(act_last_get_ts));
    resp.set("count", Json((long long)arr.size()));
    resp.set("events", arr);
    if (conn != INVALID_SOCKET) send_json(conn, resp);

    const double now = now_ts();
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "已向「%s」提供 %s 至 %s 的局域网活动信息（共 %lld 条）",
                  rname.c_str(), fmt_time(since).c_str(), fmt_time(now).c_str(), n);
    const std::string body(buf);
    emit("log", {Json(act_line(now, body))});
    record_activity("log", body);
}

// ============================================================== 循环 ==

void PeerCore::act_loop() {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // 等 UDP 线程就绪

    bool first_tick = true;
    while (running) {
        try {
            const double now = now_ts();
            bool do_hello = false, do_pull = false;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx);
                if (act_run_since <= 0) act_run_since = now_ts();
                const Cfg& c = act_cfg();

                const bool want = first_tick || (now - act_last_hello >= c.act_hello_interval);
                const bool allow = !act_events.empty() || in_net();   // 无数据且未入网不广播
                if (want && allow) {
                    act_last_hello = now;
                    do_hello = true;
                    if (first_tick && !act_events.empty()) {          // 有本机数据：开启补齐窗口
                        act_haves.clear();
                        act_pulling = true;
                        act_pull_deadline = now + c.act_pull_wait;
                    }
                }
                if (act_pulling && now >= act_pull_deadline) {
                    act_pulling = false;
                    do_pull = true;
                }
            }
            first_tick = false;

            if (do_hello) act_hello_broadcast();
            if (do_pull) act_try_pull();
        } catch (...) {
            // 网络抖动 / JSON 异常不影响循环
        }
        for (int i = 0; i < 10 && running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace nw
