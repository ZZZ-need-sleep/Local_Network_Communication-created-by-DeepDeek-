// peercore_net.cpp —— 内网准入（局域网群组）：登记表、附近内网、入网投票
// 严格复刻 neiwang.py 第 1298–2035 行（PeerCore 内网部分）与 docs/spec/02_files_innernet.md。
// 协议完全兼容：UDP 报文键名、字段类型、阈值、文案、事件顺序全部一一对应。
#include "core.h"

#include "../common/crypto.h"   // random_hex_upper
#include "../common/util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace nw {

namespace {

// ---------------------------------------------------------------- JSON 小工具 --
// Json 没有 operator[]，这里集中包装一遍，保持与 Python 的 dict 访问同形。
inline Json jget(const Json& j, const char* k) { return j.isObj() ? j.get(k) : Json(); }

inline void jset(Json& j, const char* k, const Json& v) { j.set(k, v); }

inline std::string jstr(const Json& j, const char* k, const std::string& def = "") {
    return jget(j, k).asStr(def);
}

inline double jnum(const Json& j, const char* k, double def = 0.0) {
    return jget(j, k).asNum(def);
}

inline int jint(const Json& j, const char* k, int def = 0) {
    Json v = jget(j, k);
    if (!v.isNum()) return def;
    return static_cast<int>(v.asInt(def));
}

// Python 的 `x or default`：null / 缺失 / 0 / "" / false 都取默认值。
inline std::string jstr_or(const Json& j, const char* k, const std::string& def) {
    Json v = jget(j, k);
    if (!v.isStr() || v.asStr("").empty()) return def;
    return v.asStr("");
}

inline double jnum_or(const Json& j, const char* k, double def) {
    Json v = jget(j, k);
    if (!v.isNum()) return def;
    double d = v.asNum(0.0);
    return d != 0.0 ? d : def;
}

// bool(msg.get(k) or False)：数字 1/0、true/false 都容错（Python bool() 语义）
inline bool jbool(const Json& j, const char* k) {
    Json v = jget(j, k);
    if (v.isBool()) return v.asBool(false);
    if (v.isNum()) return v.asNum(0.0) != 0.0;
    if (v.isStr()) return !v.asStr("").empty();
    return false;
}

inline Json jarr_of_str(const std::vector<std::string>& v) {
    Json a = Json::arr();
    for (const auto& s : v) a.push(Json(s));
    return a;
}

inline std::vector<std::string> str_list(const Json& v) {
    std::vector<std::string> out;
    if (!v.isArr()) return out;
    for (const auto& it : v.items()) {
        if (it.isStr()) out.push_back(it.asStr(""));
    }
    return out;
}

// sorted(set(...))：去重 + 字符串码点序（全大写十六进制下与字节序一致）
inline std::vector<std::string> sorted_unique(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

inline std::vector<std::string> voters_of(const Json& req) { return str_list(jget(req, "voters")); }

inline bool has_str(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// core.h 里 net_members 是 map<string, Json>（值就是 Python 的成员 dict：
// {mac,name,ip,tcp_port,joined_ts,founder}），落盘 / 广播时直接取 value 即可。

// ---------------------------------------------------------------- 三态 approve --
const int V_APPROVE = 1;   // True
const int V_REJECT  = 0;   // False
const int V_ABSTAIN = -1;  // None（必须是 JSON null）

inline Json vote_json(int v) {
    if (v == V_ABSTAIN) return Json(nullptr);   // 弃权 -> null，绝不能写成 false
    return Json(v == V_APPROVE);
}

// 把 JSON 里的 approve 三态解出来（缺失 / null / true / false / 1 / 0 / "true" 都能容错）
inline int vote_state(const Json& approve) {
    if (approve.isNull()) return V_ABSTAIN;
    if (approve.isBool()) return approve.asBool(false) ? V_APPROVE : V_REJECT;
    if (approve.isNum()) return approve.asNum(0.0) != 0.0 ? V_APPROVE : V_REJECT;
    if (approve.isStr()) return approve.asStr("").empty() ? V_REJECT : V_APPROVE;
    return V_ABSTAIN;
}

// ---------------------------------------------------------------- 内网门槛 --
// 规格要求：内网门槛只来自 NET_* 环境变量 / Cfg 默认值（绝不读 cfg.json 里的
// rename_vote_need 之流）。core.h 里 Cfg 是普通结构体，拿不到 PeerCore 的实例常量，
// 因此这里用「只加载一次 NET_* 覆盖」的静态缓存。
Cfg& net_cfg() {
    static Cfg c;
    static bool inited = false;
    if (!inited) {
        load_env_overrides(c);
        inited = true;
    }
    return c;
}

// net_scan 返回列表 / bootstrap 用：保持 net_nearby 的 map（= Python 字典插入）顺序
std::vector<Json> nearby_in_ttl(PeerCore* self) {
    std::vector<Json> nets;
    double now = now_ts();
    for (const auto& kv : self->net_nearby) {
        if (now - jnum(kv.second, "ts", 0.0) <= net_cfg().net_nearby_ttl)
            nets.push_back(kv.second);
    }
    return nets;
}

} // namespace

// =============================================================== 路径 / 落盘 ==
std::string PeerCore::net_path() { return path_join(data_root, "net.json"); }

void PeerCore::load_net() {
    try {
        std::string p = net_path();
        if (!file_exists(p)) return;
        std::string text;
        if (!read_file_utf8(p, text)) return;
        bool ok = false;
        Json d = Json::parse(text, &ok);
        if (!ok || !d.isObj()) return;
        Json netv = d.get("net");
        if (netv.isNull() || !netv.isObj() || !netv.has("nid")) return;

        net = netv;
        net_members.clear();
        Json ms = d.get("members");
        if (ms.isArr()) {
            for (const auto& m : ms.items()) {
                if (!m.isObj()) continue;
                std::string mac = m.get("mac").asStr("");
                if (mac.empty()) continue;
                net_members[mac] = m;   // 原样保留成员 dict
            }
        }
        net_reqs.clear();
        Json rs = d.get("reqs");
        if (rs.isArr()) {
            for (const auto& r : rs.items()) {
                if (r.isObj() && r.has("req_id") && r.get("req_id").asStr("").size())
                    net_reqs[r.get("req_id").asStr("")] = r;
            }
        }
        net_my_votes.clear();
        Json mv = d.get("my_votes");
        if (mv.isObj()) {
            for (const auto& k : mv.keys()) net_my_votes[k] = mv.get(k);
        }
        Json ap = d.get("applying");
        net_applying = (ap.isNull() || !ap.isObj()) ? Json(nullptr) : ap;

        // 立刻用本机信息刷新自己的登记项（Python 第 1321–1322 行）
        std::string my_ip = local_ipv4();   // 可路由优先（跳过 169.254.x），与 Python 版对齐
        net_add_member(uid, my_name(), my_ip, my_tcp_port(), false);
    } catch (...) {
        // Python 版这里是 traceback.print_exc() 后继续；C++ 版静默容错，保持启动不崩。
    }
}

void PeerCore::save_net() {
    try {
        Json d = Json::obj();
        d.set("net", net);
        Json members = Json::arr();
        for (const auto& kv : net_members) members.push(kv.second);
        d.set("members", members);
        Json reqs = Json::arr();
        for (const auto& kv : net_reqs) reqs.push(kv.second);
        d.set("reqs", reqs);
        Json myv = Json::obj();
        for (const auto& kv : net_my_votes) myv.set(kv.first, kv.second);
        d.set("my_votes", myv);
        d.set("applying", net_applying);
        // Python 是 indent=1；C++ 的 dump(true) 用 2 空格，键名与结构完全一致。
        write_file_utf8(net_path(), d.dump(true));
    } catch (...) {
        // Python 版同样静默吞掉写盘异常
    }
}

// =============================================================== 准入闸门 ==
bool PeerCore::in_net() const {
    if (net.isNull() || !net.isObj()) return false;
    return !net.get("nid").asStr("").empty();
}

bool PeerCore::member_ok(const std::string& id) {
    if (!in_net()) return true;
    if (id.empty()) return true;
    return id == uid || net_members.count(id) > 0;
}

// =============================================================== 查询（GUI）==
Json PeerCore::net_view() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!in_net()) {
        Json out = Json::obj();
        out.set("in_net", Json(false));
        out.set("applying", (net_applying.isObj()) ? net_applying : Json(nullptr));
        Json nearby = Json::arr();
        for (const auto& n : net_nearby_list()) nearby.push(n);
        out.set("nearby", nearby);
        return out;
    }
    std::vector<Json> members;
    for (const auto& kv : net_members) members.push_back(kv.second);
    std::stable_sort(members.begin(), members.end(), [](const Json& a, const Json& b) {
        bool af = jbool(a, "founder"), bf = jbool(b, "founder");
        if (af != bf) return af;   // 发起者在前
        return jnum(a, "joined_ts", 0.0) < jnum(b, "joined_ts", 0.0);
    });
    long long online = 0;
    for (const auto& m : members) {
        std::string mac = jstr(m, "mac");
        if (mac == uid || is_online(mac)) ++online;
    }
    Json out = Json::obj();
    out.set("in_net", Json(true));
    out.set("nid", net.get("nid"));
    out.set("name", net.get("name"));
    out.set("founder_mac", net.get("founder_mac").isStr() ? net.get("founder_mac") : Json(""));
    out.set("is_founder", Json(net.get("founder_mac").asStr("") == uid));
    out.set("my_mac", Json(uid));
    out.set("my_mac_pretty", Json(fmt_mac(uid)));
    out.set("member_count", Json((long long)members.size()));
    Json mj = Json::arr();
    for (const auto& m : members) mj.push(m);
    out.set("members", mj);
    out.set("online_count", Json(online));
    out.set("created_ts", net.get("created_ts").isNum() ? net.get("created_ts") : Json((long long)0));
    return out;
}

std::vector<Json> PeerCore::net_nearby_list() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::vector<Json> out;
    for (const auto& kv : net_nearby) out.push_back(kv.second);
    std::stable_sort(out.begin(), out.end(), [](const Json& a, const Json& b) {
        int ac = jint(a, "member_count", 0), bc = jint(b, "member_count", 0);
        if (ac != bc) return ac > bc;   // -member_count 升序 == member_count 降序
        return jstr(a, "nid") < jstr(b, "nid");
    });
    return out;
}

Json PeerCore::req_progress(Json req) {
    std::vector<std::string> voters = voters_of(req);
    Json votes = jget(req, "votes");
    std::vector<std::string> approvals, rejects;
    int abstains = 0;
    for (const auto& m : voters) {
        Json v = votes.isObj() ? votes.get(m) : Json();
        if (!v.isObj()) continue;                 // votes.get(m) or {} —— 没票不算
        int st = vote_state(jget(v, "approve"));
        if (st == V_APPROVE) approvals.push_back(m);
        else if (st == V_REJECT) rejects.push_back(m);
        else abstains++;                          // key 存在且 approve 为 null
    }
    int voters_n = static_cast<int>(voters.size());
    std::pair<int, std::string> nd = net_vote_need(voters_n, net_cfg());
    Json out = Json::obj();
    out.set("voters", Json((long long)voters_n));
    out.set("need", Json((long long)nd.first));
    out.set("desc", Json(nd.second));
    out.set("approvals", Json((long long)approvals.size()));
    out.set("rejects", Json((long long)rejects.size()));
    out.set("abstains", Json((long long)abstains));
    out.set("approvers", jarr_of_str(approvals));
    out.set("rejecters", jarr_of_str(rejects));
    return out;
}

Json PeerCore::req_card(const std::string& rid, Json req) {
    Json prog = req_progress(req);
    Json mv;
    bool voted = false;
    {
        auto it = net_my_votes.find(rid);
        if (it != net_my_votes.end()) {
            mv = it->second;
            voted = true;
        }
    }
    double now = now_ts();
    double deadline = jnum(req, "deadline", 0.0);
    double create = jnum(req, "created_ts", 0.0);
    double mv_ts = voted ? jnum(mv, "ts", 0.0) : 0.0;
    double remain = deadline - now;
    if (remain < 0.0) remain = 0.0;
    double change_left = 0.0;
    if (voted) {
        change_left = net_cfg().net_vote_change - (now - mv_ts);
        if (change_left < 0.0) change_left = 0.0;
    }
    Json card = Json::obj();
    card.set("req_id", Json(rid));
    card.set("name", Json(jstr_or(req, "name", "?")));
    card.set("mac", Json(jstr(req, "mac")));
    card.set("mac_pretty", Json(fmt_mac(jstr(req, "mac"))));
    card.set("ip", Json(jstr(req, "ip")));
    card.set("state", Json(jstr_or(req, "state", "pending")));
    card.set("note", Json(jstr(req, "note")));
    card.set("created_ts", Json(create));
    card.set("deadline", Json(deadline));
    card.set("remain", Json(remain));
    card.set("from_me", Json(jstr(req, "mac") == uid));
    card.set("voted", Json(voted));
    card.set("my_choice", voted ? vote_json(vote_state(jget(mv, "approve"))) : Json(nullptr));
    card.set("my_ts", Json(mv_ts));
    card.set("change_left", Json(change_left));
    for (const auto& k : prog.keys()) card.set(k, prog.get(k));
    return card;
}

std::vector<Json> PeerCore::net_pending_for_me() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!in_net()) return {};
    std::string my_nid = net.get("nid").asStr("");
    std::vector<Json> out;
    for (const auto& kv : net_reqs) {
        const std::string& rid = kv.first;
        Json req = kv.second;
        if (jstr(req, "state") != "pending") continue;
        if (jstr(req, "nid") != my_nid) continue;
        if (jstr(req, "mac") == uid) continue;             // 我自己发起的申请
        if (!has_str(voters_of(req), uid)) continue;
        if (net_my_votes.count(rid)) continue;             // 已投过
        out.push_back(req_card(rid, req));
    }
    std::stable_sort(out.begin(), out.end(), [](const Json& a, const Json& b) {
        return jnum(a, "created_ts", 0.0) > jnum(b, "created_ts", 0.0);
    });
    return out;
}

std::vector<Json> PeerCore::net_open_votes() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::vector<Json> out;
    if (!in_net()) {
        for (const auto& kv : net_reqs) {
            if (jstr(kv.second, "state") == "pending" && jstr(kv.second, "mac") == uid)
                out.push_back(req_card(kv.first, kv.second));
        }
        return out;
    }
    std::string my_nid = net.get("nid").asStr("");
    for (const auto& kv : net_reqs) {
        if (jstr(kv.second, "nid") != my_nid) continue;
        if (jstr(kv.second, "state") != "pending") continue;
        out.push_back(req_card(kv.first, kv.second));
    }
    std::stable_sort(out.begin(), out.end(), [](const Json& a, const Json& b) {
        return jnum(a, "created_ts", 0.0) > jnum(b, "created_ts", 0.0);
    });
    return out;
}

std::vector<Json> PeerCore::net_vote_history() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::vector<Json> out;
    for (const auto& kv : net_reqs) {
        Json card = req_card(kv.first, kv.second);
        bool can_change = jget(card, "voted").asBool(false) &&
                          jstr(card, "state") == "pending" &&
                          jnum(card, "change_left", 0.0) > 0.0;
        card.set("can_change", Json(can_change));
        out.push_back(card);
    }
    std::stable_sort(out.begin(), out.end(), [](const Json& a, const Json& b) {
        return jnum(a, "created_ts", 0.0) > jnum(b, "created_ts", 0.0);
    });
    return out;
}

// =============================================================== 建立 / 退出 ==
Json PeerCore::net_create(const std::string& name_in) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (in_net()) throw std::runtime_error("已经在内网里了");
    net_suppress_auto = false;
    std::string fallback = hostname_utf8() + " 的内网";
    std::string name = sanitize_name(name_in.empty() ? fallback : name_in, 30);
    std::string nid = random_hex_upper(3);   // secrets.token_hex(3).upper()
    Json n = Json::obj();
    n.set("nid", Json(nid));
    n.set("name", Json(name));
    n.set("founder_mac", Json(uid));
    n.set("created_ts", Json(now_ts()));
    net = n;
    net_members.clear();
    std::string my_ip = local_ipv4();   // 可路由优先（跳过 169.254.x），与 Python 版对齐
    net_add_member(uid, my_name(), my_ip, my_tcp_port(), true);
    save_net();
    net_announce();
    net_sync();
    broadcast_log("「" + my_name() + "」发起内网「" + name + "」（物理地址 " + fmt_mac(uid) +
                  "），等待同学加入");
    emit("net_status", {Json("附近没有发现内网，已自行建立内网「" + name + "」")});
    emit("net_changed", {});
    return net;
}

void PeerCore::net_leave() {
    if (!in_net()) return;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        std::string nid = net.get("nid").asStr("");
        std::string name = net.get("name").asStr("");
        Json msg = Json::obj();
        msg.set("t", Json("net_member_left"));
        msg.set("nid", Json(nid));
        msg.set("mac", Json(uid));
        msg.set("name", Json(my_name()));
        broadcast(msg);
        broadcast_log("「" + my_name() + "」退出了内网「" + name + "」");
        net = Json(nullptr);
        net_members.clear();
        net_reqs.clear();
        net_applying = Json(nullptr);
        net_my_votes.clear();
        net_suppress_auto = true;   // 手动退出后不再自动加回去
        save_net();
    }
    emit("net_status", {Json("已退出内网：可在「内网」页手动申请加入其它内网，或自行建立内网")});
    emit("net_changed", {});
    std::thread([this]() { net_rescan_after_leave(); }).detach();
}

void PeerCore::net_rescan_after_leave() {
    try {
        if (!running.load()) return;
        net_scan(net_cfg().net_scan_window);
        emit("net_changed", {});
    } catch (...) {
        // 后台线程，异常不能逃逸
    }
}

void PeerCore::net_rename(const std::string& name_in) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!in_net()) throw std::runtime_error("还没有加入内网");
    if (net.get("founder_mac").asStr("") != uid)
        throw std::runtime_error("只有内网的发起者可以重命名内网");
    std::string name = sanitize_name(name_in, 30);
    std::string old = net.get("name").asStr("");
    net.set("name", Json(name));
    save_net();
    net_announce();
    net_sync();
    broadcast_log("内网「" + old + "」更名为「" + name + "」");
    emit("net_changed", {});
}

// =============================================================== 扫描 / 申请 ==
std::vector<Json> PeerCore::net_scan(double seconds) {
    double window = (seconds <= 0.0) ? net_cfg().net_scan_window : seconds;
    double deadline = now_ts() + std::max(0.5, window);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "正在扫描附近的内网（%.0f 秒）…", window);
    emit("net_status", {Json(std::string(buf))});
    while (running.load() && now_ts() < deadline) {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        Json msg = Json::obj();
        msg.set("t", Json("net_scan"));
        msg.set("ts", Json(now_ts()));
        broadcast(msg);
        std::this_thread::sleep_for(std::chrono::duration<double>(1.0));
    }
    std::vector<Json> nets;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        nets = nearby_in_ttl(this);   // 未排序：net_nearby 的 map 顺序（= Python 字典插入序）
    }
    emit("net_nearby_changed", {});
    return nets;
}

Json PeerCore::net_apply_join(const std::string& nid) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (in_net()) throw std::runtime_error("已经在内网里了");
    net_suppress_auto = false;
    auto it = net_nearby.find(nid);
    if (it == net_nearby.end()) throw std::runtime_error("附近没有这个内网（请重新扫描）");
    Json info = it->second;
    std::vector<std::string> voters = sorted_unique(str_list(jget(info, "macs")));
    char idbuf[160];
    std::snprintf(idbuf, sizeof(idbuf), "%s-%lld-%s", uid.c_str(), msg_counter.fetch_add(1) + 1,
                  to_lower_ascii(random_hex_upper(3)).c_str());
    std::string req_id(idbuf);
    double created = now_ts();
    double deadline = created + net_cfg().net_vote_window;

    Json ap = Json::obj();
    ap.set("req_id", Json(req_id));
    ap.set("nid", Json(nid));
    ap.set("name", Json(jstr(info, "name")));
    ap.set("voters", jarr_of_str(voters));
    ap.set("created_ts", Json(created));
    ap.set("deadline", Json(deadline));
    net_applying = ap;

    std::string my_ip = local_ipv4();   // 可路由优先（跳过 169.254.x），与 Python 版对齐
    Json req = Json::obj();
    req.set("req_id", Json(req_id));
    req.set("nid", Json(nid));
    req.set("mac", Json(uid));
    req.set("name", Json(my_name()));
    req.set("ip", Json(my_ip));
    req.set("tcp_port", Json((long long)my_tcp_port()));
    req.set("voters", jarr_of_str(voters));
    req.set("votes", Json::obj());
    req.set("state", Json("pending"));
    req.set("note", Json(""));
    req.set("created_ts", Json(created));
    req.set("deadline", Json(deadline));
    net_reqs[req_id] = req;

    save_net();
    net_last_req = 0.0;
    net_join_req_broadcast();
    std::pair<int, std::string> nd = net_vote_need(static_cast<int>(voters.size()), net_cfg());
    if (nd.first == 0)
        emit("net_status", {Json("已提交申请：" + nd.second + "，等待成员确认…")});
    else
        emit("net_status", {Json("已提交申请：" + nd.second + "，等待成员投票（1 天内有效）")});
    emit("net_vote_changed", {});
    emit("net_changed", {});
    return net_applying;
}

void PeerCore::net_cancel_apply() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!net_applying.isObj()) return;
    std::string rid = jstr(net_applying, "req_id");
    if (!rid.empty()) net_reqs.erase(rid);
    net_applying = Json(nullptr);
    save_net();
    emit("net_status", {Json("已撤回入网申请")});
    emit("net_vote_changed", {});
    emit("net_changed", {});
}

// =============================================================== 投票 ==
void PeerCore::net_cast_vote(const std::string& req_id, int approve) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    auto it = net_reqs.find(req_id);
    if (it == net_reqs.end()) throw std::runtime_error("找不到该投票");
    Json req = it->second;
    if (jstr(req, "state") != "pending") throw std::runtime_error("该投票已经结束");
    if (!in_net() || jstr(req, "nid") != net.get("nid").asStr(""))
        throw std::runtime_error("只有内网成员才能投票");
    if (!has_str(voters_of(req), uid)) throw std::runtime_error("你没有本次投票权");
    int state = (approve == 1) ? V_APPROVE : (approve == 0 ? V_REJECT : V_ABSTAIN);

    auto pv = net_my_votes.find(req_id);
    if (pv != net_my_votes.end()) {
        double pts = jnum(pv->second, "ts", 0.0);
        if (now_ts() - pts > net_cfg().net_vote_change) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "投票已超过 %d 分钟，不能再改票",
                          static_cast<int>(net_cfg().net_vote_change / 60.0));
            throw std::runtime_error(std::string(buf));
        }
    }
    double ts = now_ts();
    Json mine = Json::obj();
    mine.set("approve", vote_json(state));
    mine.set("ts", Json(ts));
    net_my_votes[req_id] = mine;

    Json votes = jget(req, "votes");
    if (!votes.isObj()) votes = Json::obj();
    Json entry = Json::obj();
    entry.set("approve", vote_json(state));
    entry.set("ts", Json(ts));
    entry.set("name", Json(my_name()));
    votes.set(uid, entry);
    req.set("votes", votes);
    it->second = req;

    save_net();
    {
        Json msg = Json::obj();
        msg.set("t", Json("net_join_vote"));
        msg.set("req_id", Json(req_id));
        msg.set("nid", req.get("nid"));
        msg.set("voter", Json(uid));
        msg.set("voter_name", Json(my_name()));
        msg.set("approve", vote_json(state));
        msg.set("ts", Json(ts));
        broadcast(msg);
    }
    emit("net_vote_changed", {});
    net_eval_req(req);
    net_sync();
}

// =============================================================== 广播 ==
void PeerCore::net_announce(const std::string& only_ip) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!in_net()) return;
    std::vector<std::string> macs;
    for (const auto& kv : net_members) macs.push_back(kv.first);
    std::sort(macs.begin(), macs.end());
    if (macs.size() > 64) macs.resize(64);
    Json msg = Json::obj();
    msg.set("t", Json("net_announce"));
    msg.set("nid", net.get("nid"));
    msg.set("net_name", Json(jstr(net, "name")));
    msg.set("founder_mac", Json(jstr(net, "founder_mac")));
    msg.set("member_count", Json((long long)net_members.size()));
    msg.set("macs", jarr_of_str(macs));
    msg.set("ts", Json(now_ts()));
    broadcast(msg, only_ip);
}

void PeerCore::net_sync(const std::string& only_ip) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!in_net()) return;
    std::string nid = net.get("nid").asStr("");
    double now = now_ts();
    std::vector<Json> reqs;
    for (const auto& kv : net_reqs) {
        const Json& r = kv.second;
        if (jstr(r, "nid") != nid) continue;
        if (jstr(r, "state") == "pending" ||
            now - jnum(r, "closed_ts", 0.0) <= 3600.0)
            reqs.push_back(r);
    }
    if (reqs.size() > 30) reqs.erase(reqs.begin(), reqs.end() - 30);   // reqs[-30:]
    Json members = Json::arr();
    for (const auto& kv : net_members) members.push(kv.second);
    Json rj = Json::arr();
    for (const auto& r : reqs) rj.push(r);
    Json msg = Json::obj();
    msg.set("t", Json("net_registry"));
    msg.set("nid", Json(nid));
    msg.set("net_name", Json(jstr(net, "name")));
    msg.set("founder_mac", Json(jstr(net, "founder_mac")));
    msg.set("members", members);
    msg.set("reqs", rj);
    msg.set("ts", Json(now_ts()));
    broadcast(msg, only_ip);
}

void PeerCore::net_join_req_broadcast() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!net_applying.isObj()) return;
    std::string my_ip = local_ipv4();   // 可路由优先（跳过 169.254.x），与 Python 版对齐
    Json msg = Json::obj();
    msg.set("t", Json("net_join_req"));
    msg.set("req_id", net_applying.get("req_id"));
    msg.set("nid", net_applying.get("nid"));
    msg.set("mac", Json(uid));
    msg.set("name", Json(my_name()));
    msg.set("ip", Json(my_ip));
    msg.set("tcp_port", Json((long long)my_tcp_port()));
    msg.set("voters", jget(net_applying, "voters").isArr() ? jget(net_applying, "voters")
                                                           : Json::arr());
    msg.set("created_ts", net_applying.get("created_ts"));
    msg.set("ts", Json(now_ts()));
    broadcast(msg);
}

bool PeerCore::net_add_member(const std::string& mac_in, const std::string& name,
                              const std::string& ip, int tcp_port, bool founder) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::string mac = mac_in;
    if (mac.empty()) return false;
    auto it = net_members.find(mac);
    if (it == net_members.end()) {
        bool is_founder = founder || (!net.isNull() && net.get("founder_mac").asStr("") == mac);
        Json m = Json::obj();
        m.set("mac", Json(mac));
        m.set("name", Json(name.empty() ? std::string("?") : name));
        m.set("ip", Json(ip));
        m.set("tcp_port", Json((long long)tcp_port));
        m.set("joined_ts", Json(now_ts()));
        m.set("founder", Json(is_founder));
        net_members[mac] = m;
        return true;
    }
    Json& old = it->second;
    bool changed = false;
    if (!name.empty() && jstr(old, "name") != name) {
        old.set("name", Json(name));
        changed = true;
    }
    if (!ip.empty() && jstr(old, "ip") != ip) {
        old.set("ip", Json(ip));
        changed = true;
    }
    if (tcp_port != 0 && jint(old, "tcp_port", 0) != tcp_port) {
        old.set("tcp_port", Json((long long)tcp_port));
        changed = true;
    }
    if (founder && !jbool(old, "founder")) {
        old.set("founder", Json(true));
        changed = true;
    }
    return changed;
}

// =============================================================== 收包 ==
void PeerCore::on_net_scan(Json msg, const std::string& from_ip) {
    (void)msg;
    (void)from_ip;
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!in_net()) return;
    double now = now_ts();
    if (now - net_reply_ts < 0.5) return;   // 节流：多台同扫时不刷屏
    net_reply_ts = now;
    net_announce();   // 必须广播应答（同机多实例共用 UDP 端口）
}

void PeerCore::on_net_announce(Json msg, const std::string& from_ip) {
    std::string nid = jstr(msg, "nid");
    if (nid.empty()) return;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (in_net() && nid == net.get("nid").asStr("")) return;
    }
    std::string nm = jstr_or(msg, "net_name", "内网" + nid.substr(0, 6));
    Json info = Json::obj();
    info.set("nid", Json(nid));
    info.set("name", Json(nm));
    info.set("founder_mac", Json(jstr(msg, "founder_mac")));
    info.set("member_count", Json((long long)jint(msg, "member_count", 0)));
    info.set("macs", jget(msg, "macs").isArr() ? jget(msg, "macs") : Json::arr());
    info.set("ip", Json(from_ip));
    info.set("ts", Json(now_ts()));
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        net_nearby[nid] = info;
    }
    emit("net_nearby_changed", {});
}

void PeerCore::on_net_join_req(Json msg, const std::string& from_ip) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!in_net()) return;
    if (jstr(msg, "nid") != net.get("nid").asStr("")) return;
    std::string req_id = jstr(msg, "req_id");
    std::string mac = jstr(msg, "mac");
    if (req_id.empty() || mac.empty()) return;
    if (net_members.count(mac)) {
        net_sync();   // 已是成员：广播登记表，让他直接完成加入
        return;
    }
    auto it = net_reqs.find(req_id);
    if (it != net_reqs.end() && jstr(it->second, "state") != "pending") return;
    Json req;
    if (it == net_reqs.end()) {
        double created = jnum_or(msg, "created_ts", now_ts());
        std::string name = jstr_or(msg, "name", "?");
        std::string ip = jstr_or(msg, "ip", from_ip);
        std::vector<std::string> vs = str_list(jget(msg, "voters"));
        vs.push_back(uid);
        req = Json::obj();
        req.set("req_id", Json(req_id));
        req.set("nid", msg.get("nid"));
        req.set("mac", Json(mac));
        req.set("name", Json(name));
        req.set("ip", Json(ip));
        req.set("tcp_port", Json((long long)jint(msg, "tcp_port", 0)));
        req.set("voters", jarr_of_str(sorted_unique(vs)));
        req.set("votes", Json::obj());
        req.set("state", Json("pending"));
        req.set("note", Json(""));
        req.set("created_ts", Json(created));
        req.set("deadline", Json(created + net_cfg().net_vote_window));
        net_reqs[req_id] = req;
        std::pair<int, std::string> nd =
            net_vote_need(static_cast<int>(voters_of(req).size()), net_cfg());
        emit("log", {Json("[" + fmt_time(now_ts()) + "] " + name + "（物理地址 " + fmt_mac(mac) +
                          "）申请加入内网「" + net.get("name").asStr("") + "」：" + nd.second)});
        emit("net_vote_changed", {});
    } else {
        // 合并申请方带过来的投票人名单
        std::vector<std::string> vs = voters_of(it->second);
        for (const auto& m : str_list(jget(msg, "voters"))) vs.push_back(m);
        vs.push_back(uid);
        std::vector<std::string> merged = sorted_unique(vs);
        if (merged != voters_of(it->second)) it->second.set("voters", jarr_of_str(merged));
        req = it->second;
    }
    save_net();
    net_eval_req(req);
    net_sync();
}

void PeerCore::on_net_join_vote(Json msg) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::string rid = jstr(msg, "req_id");
    auto it = net_reqs.find(rid);
    if (it == net_reqs.end() || jstr(it->second, "state") != "pending") return;
    std::string voter = jstr(msg, "voter");
    if (voter.empty()) voter = jstr(msg, "uid");
    if (voter.empty() || !has_str(voters_of(it->second), voter)) return;
    int state = vote_state(jget(msg, "approve"));
    double ts = jnum_or(msg, "ts", now_ts());
    Json req = it->second;
    Json votes = jget(req, "votes");
    if (!votes.isObj()) votes = Json::obj();
    Json old = votes.get(voter);
    if (old.isObj() && jnum(old, "ts", 0.0) >= ts) return;   // 旧票忽略
    Json entry = Json::obj();
    entry.set("approve", vote_json(state));
    entry.set("ts", Json(ts));
    entry.set("name", Json(jstr(msg, "voter_name")));
    votes.set(voter, entry);
    req.set("votes", votes);
    it->second = req;
    save_net();
    emit("net_vote_changed", {});
    net_eval_req(req);
}

void PeerCore::on_net_registry(Json msg, const std::string& from_ip) {
    (void)from_ip;
    std::string nid = jstr(msg, "nid");
    if (nid.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (in_net() && nid == net.get("nid").asStr("")) {
        // A. 我已经在这个内网里：对齐成员表 / 申请 / 投票
        bool changed = false;
        Json ms = jget(msg, "members");
        if (ms.isArr()) {
            for (const auto& m : ms.items()) {
                if (!m.isObj()) continue;
                if (net_add_member(jstr(m, "mac"), jstr(m, "name"), jstr(m, "ip"),
                                   jint(m, "tcp_port", 0), jbool(m, "founder")))
                    changed = true;
            }
        }
        Json rs = jget(msg, "reqs");
        if (rs.isArr()) {
            for (const auto& r : rs.items()) {
                if (!r.isObj()) continue;
                if (merge_req(r)) changed = true;
            }
        }
        for (auto& kv : net_reqs) {
            if (jstr(kv.second, "state") == "pending" && jstr(kv.second, "nid") == nid)
                net_eval_req(kv.second);
        }
        if (changed) save_net();
        emit("net_changed", {});
        emit("net_vote_changed", {});
        return;
    }
    // B. 我还没入网：如果这是我的申请目标，同步投票进度 / 判断是否已被接纳
    if (!net_applying.isObj() || jstr(net_applying, "nid") != nid) return;
    std::string my_rid = jstr(net_applying, "req_id");
    {
        bool me_in = false;
        Json ms = jget(msg, "members");
        if (ms.isArr()) {
            for (const auto& m : ms.items()) {
                if (m.isObj() && jstr(m, "mac") == uid) {
                    me_in = true;
                    break;
                }
            }
        }
        if (me_in) {
            net_join_success(msg);
            return;
        }
    }
    bool changed = false;
    bool has_reject = false;
    std::string rejected_note;
    auto local_it = net_reqs.find(my_rid);
    Json rs = jget(msg, "reqs");
    if (rs.isArr() && local_it != net_reqs.end()) {
        Json local = local_it->second;
        for (const auto& r : rs.items()) {
            if (!r.isObj()) continue;
            if (jstr(r, "req_id") != my_rid) continue;
            Json votes = jget(local, "votes");
            if (!votes.isObj()) votes = Json::obj();
            Json rv = jget(r, "votes");
            if (rv.isObj()) {
                for (const auto& k : rv.keys()) {
                    Json v = rv.get(k);
                    Json old = votes.get(k);
                    if (!old.isObj() || jnum(v, "ts", 0.0) > jnum(old, "ts", 0.0)) {
                        votes.set(k, v);
                        changed = true;
                    }
                }
            }
            local.set("votes", votes);
            std::vector<std::string> vs = voters_of(local);
            for (const auto& m : str_list(jget(r, "voters"))) vs.push_back(m);
            std::vector<std::string> merged = sorted_unique(vs);
            if (merged != voters_of(local)) {
                local.set("voters", jarr_of_str(merged));
                changed = true;
            }
            if (jstr(r, "state") == "rejected") {
                local.set("state", Json("rejected"));
                local.set("note", Json(jstr(r, "note")));
                has_reject = true;
                std::string note = jstr(local, "note");
                rejected_note = note.empty() ? std::string("申请未通过") : note;
            }
        }
        local_it->second = local;
    }
    if (has_reject) {
        net_applying = Json(nullptr);
        save_net();
        emit("net_result", {Json(false), Json("入网申请未通过：" + rejected_note)});
        emit("net_vote_changed", {});
        emit("net_changed", {});
        return;
    }
    if (changed) {
        emit("net_vote_changed", {});
        emit("net_changed", {});
    }
}

bool PeerCore::merge_req(Json remote) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::string rid = jstr(remote, "req_id");
    if (rid.empty()) return false;
    auto it = net_reqs.find(rid);
    if (it == net_reqs.end()) {
        double created = jnum_or(remote, "created_ts", now_ts());
        Json r = Json::obj();
        r.set("req_id", Json(rid));
        r.set("nid", remote.get("nid"));
        r.set("mac", Json(jstr(remote, "mac")));
        r.set("name", Json(jstr_or(remote, "name", "?")));
        r.set("ip", Json(jstr(remote, "ip")));
        r.set("tcp_port", Json((long long)jint(remote, "tcp_port", 0)));
        r.set("voters", jarr_of_str(sorted_unique(str_list(jget(remote, "voters")))));
        Json rv = jget(remote, "votes");
        Json votes = Json::obj();
        if (rv.isObj()) {
            for (const auto& k : rv.keys()) votes.set(k, rv.get(k));
        }
        r.set("votes", votes);
        r.set("state", Json(jstr_or(remote, "state", "pending")));
        r.set("note", Json(jstr(remote, "note")));
        r.set("created_ts", Json(created));
        double dl = jnum(remote, "deadline", 0.0);
        r.set("deadline", Json(dl != 0.0 ? dl : created + net_cfg().net_vote_window));
        net_reqs[rid] = r;
        return true;
    }
    bool changed = false;
    Json local = it->second;
    std::vector<std::string> vs = voters_of(local);
    for (const auto& m : str_list(jget(remote, "voters"))) vs.push_back(m);
    std::vector<std::string> merged = sorted_unique(vs);
    if (merged != voters_of(local)) {
        local.set("voters", jarr_of_str(merged));
        changed = true;
    }
    Json votes = jget(local, "votes");
    if (!votes.isObj()) votes = Json::obj();
    Json rv = jget(remote, "votes");
    if (rv.isObj()) {
        for (const auto& k : rv.keys()) {
            Json v = rv.get(k);
            Json old = votes.get(k);
            if (!old.isObj() || jnum(v, "ts", 0.0) > jnum(old, "ts", 0.0)) {
                votes.set(k, v);
                changed = true;
            }
        }
    }
    local.set("votes", votes);
    std::string rstate = jstr(remote, "state");
    if (jstr(local, "state") == "pending" && (rstate == "approved" || rstate == "rejected")) {
        local.set("state", Json(rstate));
        local.set("note", Json(jstr(remote, "note")));
        changed = true;
    }
    it->second = local;
    return changed;
}

void PeerCore::on_net_member_left(Json msg) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!in_net() || jstr(msg, "nid") != net.get("nid").asStr("")) return;
    std::string mac = jstr(msg, "mac");
    if (mac.empty()) return;
    if (net_members.erase(mac) > 0) {
        save_net();
        std::string name = jstr_or(msg, "name", "?");
        emit("log", {Json("[" + fmt_time(now_ts()) + "] " + name + "（物理地址 " + fmt_mac(mac) +
                          "）退出了内网")});
        emit("net_changed", {});
    }
}

void PeerCore::on_net_join_result(Json msg) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (!net_applying.isObj() || jstr(msg, "req_id") != jstr(net_applying, "req_id")) return;
    if (jbool(msg, "approved")) return;
    std::string reason = jstr_or(msg, "reason", "申请未通过");
    net_applying = Json(nullptr);
    auto it = net_reqs.find(jstr(msg, "req_id"));
    if (it != net_reqs.end()) {
        it->second.set("state", Json("rejected"));
        it->second.set("note", Json(reason));
    }
    save_net();
    emit("net_result", {Json(false), Json("入网申请未通过：" + reason)});
    emit("net_vote_changed", {});
    emit("net_changed", {});
}

// =============================================================== 审批评估 ==
void PeerCore::net_eval_req(Json& req) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    if (jstr(req, "state") != "pending") return;
    if (!in_net() || jstr(req, "nid") != net.get("nid").asStr("")) return;
    if (net_members.count(jstr(req, "mac"))) {
        req.set("state", Json("approved"));
        req.set("note", Json("已是内网成员"));
        return;   // 注意：不 save_net、不 net_sync（与 Python 一致）
    }
    Json prog = req_progress(req);
    int need = static_cast<int>(jget(prog, "need").asInt(0));
    int voters = static_cast<int>(jget(prog, "voters").asInt(0));
    int approvals = static_cast<int>(jget(prog, "approvals").asInt(0));
    int rejects = static_cast<int>(jget(prog, "rejects").asInt(0));
    std::string desc = jstr(prog, "desc");
    double now = now_ts();
    if (need == 0) {
        net_approve_req(req, desc + "，自动通过");
        return;
    }
    if (approvals >= need) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "同意 %d/%d 票，已达门槛（%s）", approvals, voters,
                      desc.c_str());
        net_approve_req(req, std::string(buf));
        return;
    }
    if (rejects > 0 && need >= voters) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "有 %d 名成员反对，无法全票通过", rejects);
        net_reject_req(req, std::string(buf));
        return;
    }
    if (now > jnum(req, "deadline", 0.0)) {
        int hours = std::max(1, static_cast<int>(net_cfg().net_vote_window / 3600.0));
        char buf[256];
        std::snprintf(buf, sizeof(buf), "投票已满 %d 小时仍只有 %d 票（需 %d 票），投票结束", hours,
                      approvals, need);
        net_reject_req(req, std::string(buf));
    }
}

void PeerCore::net_approve_req(Json& req, const std::string& note) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    req.set("state", Json("approved"));
    req.set("note", Json(note));
    req.set("closed_ts", Json(now_ts()));
    net_add_member(jstr(req, "mac"), jstr(req, "name"), jstr(req, "ip"),
                   jint(req, "tcp_port", 0), false);
    save_net();
    net_sync();
    broadcast_log("「" + jstr_or(req, "name", "?") + "」（物理地址 " + fmt_mac(jstr(req, "mac")) +
                  "）获准加入内网「" + net.get("name").asStr("") + "」：" + note);
    emit("net_changed", {});
    emit("net_vote_changed", {});
}

void PeerCore::net_reject_req(Json& req, const std::string& note) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    req.set("state", Json("rejected"));
    req.set("note", Json(note));
    req.set("closed_ts", Json(now_ts()));
    save_net();
    std::vector<std::string> macs = voters_of(req);
    macs.push_back(jstr(req, "mac"));
    std::vector<std::string> uniq = sorted_unique(macs);
    if (!uniq.empty() && uniq[0] == uid) {
        // 由物理地址最小者统一宣布结果，避免重复广播
        Json msg = Json::obj();
        msg.set("t", Json("net_join_result"));
        msg.set("req_id", req.get("req_id"));
        msg.set("nid", req.get("nid"));
        msg.set("mac", req.get("mac"));
        msg.set("approved", Json(false));
        msg.set("reason", Json(note));
        msg.set("ts", Json(now_ts()));
        broadcast(msg);
    }
    emit("log", {Json("[" + fmt_time(now_ts()) + "] " + jstr_or(req, "name", "?") + "（物理地址 " +
                      fmt_mac(jstr(req, "mac")) + "）入网申请未通过：" + note)});
    emit("net_changed", {});
    emit("net_vote_changed", {});
}

void PeerCore::net_join_success(Json registry) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::string nid = jstr(registry, "nid");
    if (nid.empty() && net_applying.isObj()) nid = jstr(net_applying, "nid");
    std::string name = jstr(registry, "net_name");
    if (name.empty() && net_applying.isObj()) name = jstr(net_applying, "name");
    if (name.empty()) name = "内网" + nid.substr(0, 6);
    double now = now_ts();
    Json n = Json::obj();
    n.set("nid", Json(nid));
    n.set("name", Json(name));
    n.set("founder_mac", Json(jstr(registry, "founder_mac")));
    n.set("created_ts", Json(now));
    n.set("joined_ts", Json(now));
    net = n;
    net_members.clear();
    Json ms = jget(registry, "members");
    if (ms.isArr()) {
        for (const auto& m : ms.items()) {
            if (m.isObj() && !jstr(m, "mac").empty())
                net_add_member(jstr(m, "mac"), jstr(m, "name"), jstr(m, "ip"),
                               jint(m, "tcp_port", 0), jbool(m, "founder"));
        }
    }
    std::string my_ip = local_ipv4();   // 可路由优先（跳过 169.254.x），与 Python 版对齐
    net_add_member(uid, my_name(), my_ip, my_tcp_port(), false);
    net_applying = Json(nullptr);
    save_net();
    net_announce();
    net_sync();
    std::string nname = net.get("name").asStr("");
    broadcast_log("「" + my_name() + "」（物理地址 " + fmt_mac(uid) + "）加入了内网「" + nname + "」");
    emit("net_changed", {});
    char buf[256];
    std::snprintf(buf, sizeof(buf), "已加入内网「%s」（成员 %d 人）", nname.c_str(),
                  static_cast<int>(net_members.size()));
    emit("net_result", {Json(true), Json(std::string(buf))});
}

// =============================================================== 周期循环 ==
void PeerCore::net_loop() {
    std::this_thread::sleep_for(std::chrono::duration<double>(0.4));   // 等 UDP 线程就绪
    if (!running.load()) return;
    net_bootstrap();
    while (running.load()) {
        try {
            net_tick();
        } catch (...) {
            // Python：_running 时 traceback 并继续；C++ 静默继续
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(2.0));
    }
}

void PeerCore::net_bootstrap() {
    if (in_net()) {
        net_announce();
        net_sync();
        emit("net_changed", {});
        char buf[256];
        std::snprintf(buf, sizeof(buf), "已在内网「%s」（成员 %d 人）", net.get("name").asStr("").c_str(),
                      static_cast<int>(net_members.size()));
        emit("net_status", {Json(std::string(buf))});
        return;
    }
    if (net_applying.isObj()) {
        emit("net_status", {Json("正在等待内网成员投票…")});
        net_last_req = 0.0;
        net_join_req_broadcast();
        emit("net_changed", {});
        return;
    }
    std::vector<Json> found = net_scan(net_cfg().net_scan_window);
    if (net_suppress_auto) {
        // 用户手动退出过：只扫描不自动申请
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "已退出内网（附近发现 %d 个内网）：可在「内网」页手动加入或自行建立",
                      static_cast<int>(found.size()));
        emit("net_status", {Json(std::string(buf))});
        emit("net_changed", {});
        return;
    }
    if (!found.empty()) {
        Json best = found[0];   // 未排序：net_nearby 的插入顺序中第一个
        char buf[256];
        std::snprintf(buf, sizeof(buf), "发现内网「%s」（%d 人），正在申请加入…",
                      jstr(best, "name").c_str(), jint(best, "member_count", 0));
        emit("net_status", {Json(std::string(buf))});
        try {
            net_apply_join(jstr(best, "nid"));
        } catch (...) {
            net_create(std::string());
        }
    } else {
        net_create(std::string());
    }
}

void PeerCore::net_tick() {
    double now = now_ts();
    if (in_net()) {
        if (now - net_last_announce >= net_cfg().net_announce_interval) {
            net_last_announce = now;
            net_announce();
            net_sync();
        }
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            auto it = net_reqs.begin();
            while (it != net_reqs.end()) {
                if (jstr(it->second, "state") == "pending") net_eval_req(it->second);
                ++it;
            }
        }
        // 每 60 秒用「原始 ts」重播我的票，帮助掉线过的同学补齐
        if (now - net_last_vote_replay >= 60.0) {
            net_last_vote_replay = now;
            std::lock_guard<std::recursive_mutex> lk(mtx);
            for (const auto& kv : net_my_votes) {
                auto it = net_reqs.find(kv.first);
                if (it == net_reqs.end() || jstr(it->second, "state") != "pending") continue;
                Json msg = Json::obj();
                msg.set("t", Json("net_join_vote"));
                msg.set("req_id", Json(kv.first));
                msg.set("nid", it->second.get("nid"));
                msg.set("voter", Json(uid));
                msg.set("voter_name", Json(my_name()));
                msg.set("approve", jget(kv.second, "approve"));
                msg.set("ts", jget(kv.second, "ts"));
                broadcast(msg);
            }
        }
        net_prune();
    }
    if (net_applying.isObj()) {
        if (now - net_last_req >= net_cfg().net_req_rebroadcast) {
            net_last_req = now;
            net_join_req_broadcast();
            emit("net_vote_changed", {});
        }
        if (now > jnum(net_applying, "deadline", 0.0)) {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            std::string rid = jstr(net_applying, "req_id");
            net_applying = Json(nullptr);
            auto it = net_reqs.find(rid);
            if (it != net_reqs.end()) {
                it->second.set("state", Json("rejected"));
                it->second.set("note", Json("投票超过 1 天，自动结束"));
            }
            save_net();
            emit("net_result", {Json(false), Json("入网申请超过 1 天仍未被通过，投票已结束")});
            emit("net_changed", {});
        }
    }
    {
        std::vector<std::string> drop;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            for (const auto& kv : net_nearby) {
                if (now - jnum(kv.second, "ts", 0.0) > net_cfg().net_nearby_ttl)
                    drop.push_back(kv.first);
            }
            for (const auto& nid : drop) net_nearby.erase(nid);
        }
        for (size_t i = 0; i < drop.size(); ++i) emit("net_nearby_changed", {});
    }
}

void PeerCore::net_prune() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    double cutoff = now_ts() - 7.0 * 86400.0;
    std::vector<std::string> drop;
    for (const auto& kv : net_reqs) {
        std::string st = jstr(kv.second, "state");
        if ((st == "approved" || st == "rejected") &&
            jnum(kv.second, "created_ts", 0.0) < cutoff)
            drop.push_back(kv.first);
    }
    if (drop.empty()) return;
    for (const auto& rid : drop) {
        net_reqs.erase(rid);
        net_my_votes.erase(rid);
    }
    save_net();
}

} // namespace nw
