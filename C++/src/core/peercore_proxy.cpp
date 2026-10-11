// peercore_proxy.cpp —— v2.0.0 设备数据传输代理（新功能 B）
//
// 严格对齐 neiwang.py 的代理实现与 docs/design_v2.md 第 2 节：
//   proxy_dir / proxy_path / load_proxies / save_proxies
//   on_proxy_req / on_proxy_ack / on_proxy_online / on_proxy_consumed
//   proxy_on_peer_online / proxy_push_to / proxy_store_received / proxy_push_received
//   proxy_store_local / proxy_loop / proxy_list / proxy_start / proxy_delete
//   send_group_text_with_proxy
//
//  · 数据目录：data/proxy/（文件以 .proxy 结尾）与 data/proxy.json（元数据）
//  · 代理记录绝不产生 UI 列表项 / 弹窗，只在「日志」页与活动历史里体现
//  · 日志行一律 emit_log("[时间] …") + record_activity(kind, 无时间前缀的正文)
//    （与 Python 的 _emit_act_log(text, kind) 完全一致）
//  · TCP 应答帧由本文件自己发送（core.h 的 proxy_store_received/proxy_push_received
//    已带 SOCKET conn 参数），无需其它文件配合回包

#include "core.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../common/crypto.h"   // random_hex_upper
#include "../common/sha2.h"     // Sha256Ctx
#include "../common/util.h"

namespace nw {

namespace {

// ---------------------------------------------------------------- 配置缓存 --
Cfg& proxy_cfg() {
    static Cfg c;
    static bool inited = false;
    if (!inited) { load_env_overrides(c); inited = true; }
    return c;
}

// ---------------------------------------------------------------- JSON 小工具 --
Json jget(const Json& j, const char* k) { return j.isObj() ? j.get(k) : Json(); }

std::string jstr(const Json& j, const char* k, const std::string& def = "") {
    return jget(j, k).asStr(def);
}

// Python 的 `x or default`：缺失 / null / "" 取默认值
std::string jstr_or(const Json& j, const char* k, const std::string& def) {
    Json v = jget(j, k);
    if (v.isNull()) return def;
    std::string s = v.asStr("");
    return s.empty() ? def : s;
}

long long jint(const Json& j, const char* k, long long def = 0) {
    Json v = jget(j, k);
    return v.isNum() ? v.asInt(def) : def;
}

std::vector<std::string> jstr_list(const Json& j, const char* k) {
    std::vector<std::string> out;
    for (const Json& x : jget(j, k).items()) {
        if (x.isStr()) out.push_back(x.asStr());
    }
    return out;
}

Json str_array(const std::vector<std::string>& v) {
    Json a = Json::arr();
    for (const auto& s : v) a.push(Json(s));
    return a;
}

bool has_str(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

void add_unique(std::vector<std::string>& v, const std::string& s) {
    if (!s.empty() && !has_str(v, s)) v.push_back(s);
}

// 按字节截断但保证不切开一个 UTF-8 字符（等价 Python 的 str[:n]）
std::string utf8_prefix_keep(const std::string& s, size_t maxlen) {
    if (maxlen == 0 || s.size() <= maxlen) return maxlen == 0 ? std::string() : s;
    size_t n = maxlen;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

// ------------------------------------------------- 运行期：还没有 target 上线 --
// 对应 Python 的 self._proxy_no_target（proxy_id 集合）。core.h 没有该成员，
// 这里用「按实例索引」的文件级集合实现同一语义，不污染 core.h。
std::mutex& no_target_mtx() {
    static std::mutex m;
    return m;
}
std::map<const PeerCore*, std::set<std::string>>& no_target_map() {
    static std::map<const PeerCore*, std::set<std::string>> m;
    return m;
}
void no_target_add(const PeerCore* pc, const std::string& pid) {
    std::lock_guard<std::mutex> lk(no_target_mtx());
    no_target_map()[pc].insert(pid);
}
void no_target_discard(const PeerCore* pc, const std::string& pid) {
    std::lock_guard<std::mutex> lk(no_target_mtx());
    auto it = no_target_map().find(pc);
    if (it != no_target_map().end()) it->second.erase(pid);
}
bool no_target_has(const PeerCore* pc, const std::string& pid) {
    std::lock_guard<std::mutex> lk(no_target_mtx());
    auto it = no_target_map().find(pc);
    return it != no_target_map().end() && it->second.count(pid) > 0;
}

// ---------------------------------------------------------------- 路径/日志 --
std::string proxy_file_path(PeerCore& pc, const std::string& pid) {
    return path_join(pc.proxy_dir(), pid + ".proxy");
}

// Python 的 _emit_act_log(text, kind)：日志页整行带时间，活动历史只存正文
void emit_act_log(PeerCore& pc, const std::string& text, double ts, const char* kind = "proxy") {
    pc.emit_log("[" + fmt_time(ts) + "] " + text, ts);
    pc.record_activity(kind, text, ts);
}

void record_consumed_activity(PeerCore& pc, const std::string& pid, const std::string& holder) {
    pc.record_activity("proxy", "代理已接受 proxy_id=" + pid + " holder=" + holder);
}

std::string net_nid(PeerCore& pc) {
    std::lock_guard<std::recursive_mutex> lk(pc.mtx);
    return pc.net.isObj() ? pc.net.get("nid").asStr() : std::string();
}

// ---------------------------------------------------------------- 记录转换 --
std::string kindname_of(const std::string& kind) {
    return kind == "file" ? "文件代理" : "信息代理";
}

// Python 的 fname = name or text
std::string fname_of(const ProxyRec& r) { return r.name.empty() ? r.text : r.name; }

ProxyRec rec_from_json(const Json& j) {
    ProxyRec r;
    r.proxy_id = jstr(j, "proxy_id");
    r.kind = jstr_or(j, "kind", "file");
    r.gid = jstr(j, "gid");
    r.gname = jstr(j, "gname");
    r.file_id = jstr(j, "file_id");
    r.name = jstr(j, "name");
    r.size = jint(j, "size", 0);
    r.text = jstr(j, "text");
    r.sender_uid = jstr(j, "sender_uid");
    r.sender_name = jstr(j, "sender_name");
    r.from_mac = jstr(j, "from_mac");
    r.created_ts = jget(j, "created_ts").asNum(0.0);
    r.ts = jget(j, "ts").asNum(0.0);
    r.targets = jstr_list(j, "targets");
    r.delivered_to = jstr_list(j, "delivered_to");
    r.holders = jstr_list(j, "holders");
    r.consumed = jget(j, "consumed").asBool(false);
    r.holder = jstr(j, "holder");
    return r;
}

Json rec_to_json(const ProxyRec& r) {
    Json o = Json::obj();
    o.set("proxy_id", Json(r.proxy_id));
    o.set("kind", Json(r.kind));
    o.set("gid", Json(r.gid));
    o.set("gname", Json(r.gname));
    o.set("file_id", Json(r.file_id));
    o.set("name", Json(r.name));
    o.set("size", Json((long long)r.size));
    o.set("text", Json(r.text));
    o.set("sender_uid", Json(r.sender_uid));
    o.set("sender_name", Json(r.sender_name));
    o.set("from_mac", Json(r.from_mac));
    o.set("created_ts", Json(r.created_ts));
    o.set("ts", Json(r.ts));
    o.set("targets", str_array(r.targets));
    o.set("delivered_to", str_array(r.delivered_to));
    o.set("holders", str_array(r.holders));
    o.set("consumed", Json(r.consumed));
    o.set("holder", Json(r.holder));
    return o;
}

// ---------------------------------------------------------------- 网络小工具 --
const size_t kProxyChunk = 256 * 1024;   // 拉取代理文件的分块

// 找人：优先在线表（peers），其次内网登记表（net_members）
bool resolve_peer(PeerCore& pc, const std::string& mac, const std::string& name, PeerInfo& out) {
    std::lock_guard<std::recursive_mutex> lk(pc.mtx);
    auto it = pc.peers.find(mac);
    if (it != pc.peers.end() && !it->second.ip.empty() && it->second.tcp_port > 0) {
        out = it->second;
        out.uid = mac;
        if (out.name.empty()) out.name = name;
        return true;
    }
    auto nm = pc.net_members.find(mac);
    if (nm != pc.net_members.end()) {
        std::string nip = nm->second.get("ip").asStr();
        int nport = (int)nm->second.get("tcp_port").asInt(0);
        if (!nip.empty() && nport > 0) {
            out.uid = mac;
            std::string nname = nm->second.get("name").asStr();
            out.name = nname.empty() ? name : nname;
            out.ip = nip;
            out.tcp_port = nport;
            return true;
        }
    }
    return false;
}

// TCP proxy_store / proxy_push 的报文（design_v2 2.4(1) / 2.4(3)）
Json proxy_tcp_payload(PeerCore& pc, const ProxyRec& r, const char* cmd) {
    Json m = Json::obj();
    m.set("cmd", Json(cmd));
    m.set("proxy_id", Json(r.proxy_id));
    m.set("kind", Json(r.kind));
    m.set("gid", Json(r.gid));
    m.set("gname", Json(r.gname));
    m.set("file_id", Json(r.file_id));
    m.set("name", Json(r.name));
    m.set("size", Json((long long)r.size));
    m.set("text", Json(r.text));
    m.set("sender_uid", Json(r.sender_uid));
    m.set("sender_name", Json(r.sender_name));
    m.set("targets", str_array(r.targets));
    m.set("ts", Json(r.ts));
    m.set("ip", Json(local_ipv4()));
    m.set("port", Json((long long)pc.my_tcp_port()));
    return m;
}

// 连接 -> 发送 -> 读一条应答；等价 Python _proxy_store_on / _proxy_push_to
bool proxy_tcp_exchange(const std::string& ip, int port, const Json& msg,
                        const std::string& want_cmd, const std::string& want_pid,
                        Json& out) {
    if (ip.empty() || port <= 0) return false;
    double t = proxy_cfg().proxy_push_timeout;
    if (t < 10.0) t = 10.0;
    t += 60.0;                                   // Python: max(PROXY_PUSH_TIMEOUT,10)+60
    int timeout_ms = (int)(t * 1000.0);
    SOCKET s = tcp_connect(ip, port, timeout_ms);
    if (s == INVALID_SOCKET) return false;
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    bool ok = false;
    if (send_json(s, msg) && recv_json(s, out)) {
        bool ok_field = true;
        if (out.get("ok").isBool()) ok_field = out.get("ok").asBool();
        ok = (out.get("cmd").asStr() == want_cmd) &&
             (want_pid.empty() || out.get("proxy_id").asStr() == want_pid) &&
             ok_field;
    }
    close_socket(s);
    return ok;
}

bool proxy_store_on(PeerCore& pc, const PeerInfo& peer, const ProxyRec& rec) {
    Json msg = proxy_tcp_payload(pc, rec, "proxy_store");
    Json resp;
    return proxy_tcp_exchange(peer.ip, peer.tcp_port, msg, "proxy_stored", rec.proxy_id, resp);
}

// 用 cmd=get 从寄存方 / 推送方拉取代理文件内容，流式写入 dest 并校验 SHA-256。
// from_push=true 时带 proxy_id 与 from=push（design_v2 2.4(2)/(3)）。
bool pull_proxy_file(PeerCore& pc, const std::string& ip, int port,
                     const std::string& file_id, const std::string& proxy_id,
                     bool from_push, const std::string& dest, std::string& err) {
    err.clear();
    if (ip.empty() || port <= 0 || file_id.empty()) { err = "缺少地址或文件指纹"; return false; }
    SOCKET s = tcp_connect(ip, port, 60000);
    if (s == INVALID_SOCKET) { err = "无法连接寄存方"; return false; }

    Json req = Json::obj();
    req.set("cmd", Json("get"));
    req.set("file_id", Json(file_id));
    if (from_push) {
        req.set("proxy_id", Json(proxy_id));
        req.set("from", Json("push"));
    }
    if (!send_json(s, req)) { close_socket(s); err = "发送请求失败"; return false; }

    DWORD tv = 60000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    Json hdr;
    if (!recv_json(s, hdr)) { close_socket(s); err = "接收应答失败"; return false; }
    if (hdr.get("cmd").asStr() != "data") {
        err = jstr_or(hdr, "err", "对方应答异常");
        close_socket(s);
        return false;
    }
    long long size = jint(hdr, "size", 0);

    std::ofstream f(u82ws(dest), std::ios::binary | std::ios::trunc);
    if (!f) { close_socket(s); err = "写入代理文件失败"; return false; }
    Sha256Ctx ctx;
    ctx.init();
    std::vector<char> buf(kProxyChunk);
    long long got = 0;
    bool io_ok = true;
    while (got < size && pc.running) {
        long long want = size - got;
        if (want > (long long)buf.size()) want = (long long)buf.size();
        int n = recv(s, buf.data(), (int)want, 0);
        if (n <= 0) { io_ok = false; break; }
        f.write(buf.data(), n);
        if (!f) { io_ok = false; break; }
        ctx.update(buf.data(), (size_t)n);
        got += n;
    }
    f.close();
    close_socket(s);
    if (!io_ok || got != size) { remove_file(dest); err = "数据不完整"; return false; }
    if (ctx.hex() != to_lower_ascii(file_id)) { remove_file(dest); err = "文件校验失败"; return false; }
    return true;
}

void broadcast_consumed(PeerCore& pc, const std::string& pid, const std::string& holder) {
    Json m = Json::obj();
    m.set("t", Json("proxy_consumed"));
    m.set("uid", Json(pc.uid));
    m.set("name", Json(pc.my_name()));
    m.set("msg_id", Json(new_msg_id(pc.uid, ++pc.msg_counter)));
    m.set("proxy_id", Json(pid));
    m.set("holder", Json(holder));
    m.set("mac", Json(pc.uid));
    m.set("ts", Json(now_ts()));
    pc.broadcast(m);
}

} // namespace

// ================================================================ 目录/元数据 ==

std::string PeerCore::proxy_dir() {
    std::string d = path_join(data_root, "proxy");
    ensure_dir(d);
    return d;
}

// core.h 无参版本：等价 Python 的 proxy_meta_path()（data/proxy.json）
std::string PeerCore::proxy_path() {
    return path_join(data_root, "proxy.json");
}

void PeerCore::load_proxies() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    proxies.clear();
    std::string txt;
    if (!file_exists(proxy_path())) return;
    if (!read_file_utf8(proxy_path(), txt)) return;
    bool ok = false;
    Json d = Json::parse(txt, &ok);
    if (!ok || !d.isObj()) return;
    for (const auto& k : d.keys()) {
        Json v = d.get(k);
        if (!v.isObj()) continue;                 // Python: 只保留 dict 项
        ProxyRec r = rec_from_json(v);
        if (r.proxy_id.empty()) r.proxy_id = k;
        proxies[k] = r;
    }
}

void PeerCore::save_proxies() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    Json o = Json::obj();
    for (const auto& kv : proxies) o.set(kv.first, rec_to_json(kv.second));
    ensure_dir(data_root);
    write_file_utf8(proxy_path(), o.dump(true));
}

Json PeerCore::proxy_list() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    Json a = Json::arr();
    for (const auto& kv : proxies) a.push(rec_to_json(kv.second));
    return a;
}

// ================================================================ UDP 收包 ==

void PeerCore::on_proxy_req(Json msg, const std::string& from_ip) {
    (void)from_ip;
    if (!in_net() || tcp_srv == INVALID_SOCKET) return;
    std::string nid = jstr(msg, "net_id");
    if (!nid.empty() && nid != net_nid(*this)) return;      // 不是同一个内网
    if (!member_ok(jstr(msg, "uid"))) return;
    std::string req_id = jstr(msg, "req_id");
    if (req_id.empty()) return;

    Json ack = Json::obj();
    ack.set("t", Json("proxy_ack"));
    ack.set("uid", Json(uid));
    ack.set("name", Json(my_name()));
    ack.set("msg_id", Json(new_msg_id(uid, ++msg_counter)));
    ack.set("req_id", Json(req_id));
    ack.set("mac", Json(uid));
    ack.set("ip", Json(local_ipv4()));
    ack.set("port", Json((long long)my_tcp_port()));
    ack.set("ts", Json(now_ts()));
    broadcast(ack);
}

void PeerCore::on_proxy_ack(Json msg) {
    std::string req_id = jstr(msg, "req_id");
    if (req_id.empty()) return;
    std::string mac = jstr(msg, "mac");
    if (mac.empty()) mac = jstr(msg, "uid");
    if (mac.empty() || mac == uid) return;

    std::string name = jstr_or(msg, "name", "?");
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        auto it = proxy_reqs.find(req_id);
        if (it == proxy_reqs.end()) return;               // 不是我在等的请求
        Json arr = it->second;
        if (!arr.isArr()) arr = Json::arr();
        for (const Json& a : arr.items()) {
            if (a.get("mac").asStr() == mac) return;      // 同一成员只记一次
        }
        Json a = Json::obj();
        a.set("mac", Json(mac));
        a.set("uid", Json(jstr(msg, "uid")));
        a.set("name", Json(name));
        a.set("ip", Json(jstr(msg, "ip")));
        a.set("port", Json((long long)jint(msg, "port", 0)));
        arr.push(a);
        it->second = arr;
    }
    emit_act_log(*this, "「" + name + "」（物理地址 " + fmt_mac(mac) + "）回复在线", now_ts());
}

void PeerCore::on_proxy_online(Json msg) {
    std::string mac = jstr(msg, "uid");
    if (mac.empty()) mac = jstr(msg, "mac");
    if (mac.empty() || mac == uid) return;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (online_known.count(mac)) return;              // 已经提醒过
    }
    if (!in_net() || !member_ok(mac)) return;

    std::string name = jstr(msg, "name");
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        auto it = peers.find(mac);
        if (it == peers.end()) return;                    // 还没有 presence 地址信息，等 presence
        if (name.empty()) name = it->second.name;
    }
    if (name.empty()) name = "?";

    std::vector<std::string> groups;
    for (const Json& g : msg.get("groups").items()) {
        if (g.isStr()) groups.push_back(g.asStr());
    }
    // 上线提醒 + 代理处理（与 Python _on_peer_online 一致）
    proxy_on_peer_online(mac, mac, name, groups);
}

void PeerCore::on_proxy_consumed(Json msg) {
    if (!member_ok(jstr(msg, "uid"))) return;
    std::string pid = jstr(msg, "proxy_id");
    std::string holder = jstr(msg, "holder");
    if (pid.empty()) return;
    bool has = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        has = proxies.find(pid) != proxies.end();
    }
    if (!has) return;
    proxy_delete(pid, holder);
    record_consumed_activity(*this, pid, holder);
}

// ============================================================ 代理被接受 ==

bool PeerCore::proxy_delete(const std::string& proxy_id, const std::string& holder) {
    if (proxy_id.empty()) return false;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        auto it = proxies.find(proxy_id);
        if (it == proxies.end()) return false;
        if (!holder.empty() && holder == uid) {
            // 我是接受者：保留并成为唯一分发源
            it->second.consumed = true;
            it->second.holder = holder;
            it->second.holders.clear();
            it->second.holders.push_back(uid);
            save_proxies();
            return false;
        }
        proxies.erase(it);
    }
    no_target_discard(this, proxy_id);
    remove_file(proxy_file_path(*this, proxy_id));
    save_proxies();
    return true;
}

// ==================================================== 有人上线：代理三分支 ==

void PeerCore::proxy_on_peer_online(const std::string& mac, const std::string& uid_in,
                                    const std::string& name,
                                    const std::vector<std::string>& groups) {
    (void)uid_in;                       // 本项目 mac 与 uid 是同一个值，统一用 mac
    const std::string my_uid = this->uid;
    if (mac.empty() || mac == my_uid) return;
    if (!in_net() || !member_ok(mac)) return;

    // 1) 上线提醒（Python _on_peer_online；去重集合 online_known）
    {
        bool first = false;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            first = online_known.find(mac) == online_known.end();
            online_known.insert(mac);
        }
        if (first) {
            emit_act_log(*this, "上线提醒：「" + name + "」（物理地址 " + fmt_mac(mac) + "）已上线",
                         now_ts(), "sys");
        }
    }

    // 2) 快照所有未 consumed 的代理（顺带按对方声明的群补全 targets）
    std::vector<ProxyRec> work;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        for (auto& kv : proxies) {
            ProxyRec& r = kv.second;
            if (r.consumed) continue;
            if (!r.gid.empty() && has_str(groups, r.gid) && !has_str(r.targets, mac)) {
                r.targets.push_back(mac);
            }
            work.push_back(r);
        }
    }

    PeerInfo peer;
    bool have_peer = resolve_peer(*this, mac, name, peer);
    const double now = now_ts();
    const double group_wait = proxy_cfg().proxy_group_wait;

    for (const ProxyRec& r : work) {
        const std::string pid = r.proxy_id;
        if (pid.empty()) continue;
        const std::string kindname = kindname_of(r.kind);
        const std::string fname = fname_of(r);

        // 分支 1：原作者上线 -> 回传并删除本地代理
        if (mac == r.sender_uid) {
            if (!have_peer) continue;
            if (!proxy_push_to(peer, r, false)) continue;
            broadcast_consumed(*this, pid, "");
            proxy_delete(pid, "");
            emit_act_log(*this, "原作者「" + name + "」已上线，已将" + kindname + "「" + fname +
                                    "」回传并删除本地代理", now);
            record_consumed_activity(*this, pid, "");
            continue;
        }

        // 分支 2：群内成员上线 -> 送达（接受者成为新的分发源）
        if (has_str(r.targets, mac)) {
            if (!have_peer) continue;
            if (!proxy_push_to(peer, r, true)) continue;
            no_target_discard(this, pid);
            broadcast_consumed(*this, pid, mac);
            proxy_delete(pid, mac);
            emit_act_log(*this, "群「" + r.gname + "」成员「" + name + "」已上线，已将" + kindname +
                                    "「" + fname + "」送达并删除本地代理", now);
            record_consumed_activity(*this, pid, mac);
            continue;
        }

        // 分支 3：群成员一直没上线 -> 向本局域网其他成员扩散一份副本（不删除、不 consumed）
        // 已经给他扩散过的（delivered_to）不再重复推送：C++ 侧 presence 每次心跳都会调用本函数，
        // Python 侧靠 _known_online 只在「本轮上线」处理一次。
        if (now - r.created_ts >= group_wait && no_target_has(this, pid) &&
            !has_str(r.delivered_to, mac)) {
            if (!have_peer) continue;
            if (!proxy_push_to(peer, r, false)) continue;
            {
                std::lock_guard<std::recursive_mutex> lk(mtx);
                auto it = proxies.find(pid);
                if (it != proxies.end()) {
                    add_unique(it->second.delivered_to, mac);
                    add_unique(it->second.holders, mac);
                }
            }
            save_proxies();
            emit_act_log(*this, "群「" + r.gname + "」成员一直未上线，已将" + kindname + "「" + fname +
                                    "」发送给本局域网成员「" + name + "」一份", now);
        }
    }
}

// ============================================================ TCP 推送/寄存 ==

bool PeerCore::proxy_push_to(const PeerInfo& peer, const ProxyRec& rec, bool is_target) {
    // 接收方按「本机 mac ∈ targets」自行判定是否为最终接受者（design_v2 2.4(3)/2.6），
    // 因此报文里不需要 is_target 标志。
    (void)is_target;
    Json msg = proxy_tcp_payload(*this, rec, "proxy_push");
    Json resp;
    return proxy_tcp_exchange(peer.ip, peer.tcp_port, msg, "proxy_ok", rec.proxy_id, resp);
}

bool PeerCore::proxy_store_local(const ProxyRec& rec, const std::string& from_mac) {
    if (rec.proxy_id.empty()) return false;
    std::lock_guard<std::recursive_mutex> lk(mtx);
    ProxyRec r = rec;
    if (!from_mac.empty()) r.from_mac = from_mac;
    if (!has_str(r.holders, uid)) r.holders.push_back(uid);
    proxies[r.proxy_id] = r;
    save_proxies();
    return true;
}

// cmd=proxy_store：代别人寄存（kind=file 时立刻从寄存方拉文件落盘）
void PeerCore::proxy_store_received(Json req, SOCKET conn) {
    std::string pid = jstr(req, "proxy_id");
    if (pid.empty()) {
        Json reply = Json::obj();
        reply.set("cmd", Json("proxy_stored"));
        reply.set("proxy_id", Json(""));
        reply.set("ok", Json(false));
        reply.set("err", Json("缺少 proxy_id"));
        send_json(conn, reply);
        return;
    }

    std::string kind = jstr_or(req, "kind", "file");
    std::string fid = jstr(req, "file_id");
    std::string sender_uid = jstr(req, "sender_uid");
    std::string sender_name = jstr_or(req, "sender_name", "?");
    std::string fname = jstr(req, "name");
    std::string ip = jstr(req, "ip");
    long long port = jint(req, "port", 0);

    if (ip.empty() || port <= 0) {
        PeerInfo p;
        if (resolve_peer(*this, sender_uid, sender_name, p)) { ip = p.ip; port = p.tcp_port; }
    }

    if (kind == "file") {
        std::string dest = proxy_file_path(*this, pid);
        std::string err;
        if (!pull_proxy_file(*this, ip, (int)port, fid, "", false, dest, err)) {
            Json reply = Json::obj();
            reply.set("cmd", Json("proxy_stored"));
            reply.set("proxy_id", Json(pid));
            reply.set("ok", Json(false));
            reply.set("err", Json("拉取代理文件失败"));
            send_json(conn, reply);
            return;
        }
    }

    double now = now_ts();
    ProxyRec r;
    r.proxy_id = pid;
    r.kind = kind;
    r.gid = jstr(req, "gid");
    r.gname = jstr(req, "gname");
    r.file_id = fid;
    r.name = fname;
    r.size = jint(req, "size", 0);
    r.text = jstr(req, "text");
    r.sender_uid = sender_uid;
    r.sender_name = sender_name;
    r.from_mac = sender_uid;
    {
        double rts = jget(req, "ts").asNum(0.0);   // Python: float(ts or now)
        if (rts <= 0.0) rts = now;
        r.created_ts = rts;
        r.ts = rts;
    }
    r.targets = jstr_list(req, "targets");
    r.consumed = false;
    r.holder = "";
    proxy_store_local(r, sender_uid);

    Json reply = Json::obj();
    reply.set("cmd", Json("proxy_stored"));
    reply.set("proxy_id", Json(pid));
    reply.set("ok", Json(true));
    send_json(conn, reply);

    if (kind == "file") {
        emit_act_log(*this, "已代「" + sender_name + "」寄存文件代理「" + fname +
                                "」（用户不可查看，后缀已改为 .proxy）", now_ts());
    } else {
        emit_act_log(*this, "已代「" + sender_name + "」寄存信息代理（用户不可查看）", now_ts());
    }
}

// cmd=proxy_push：接收代理（kind=file 时从推送方拉取内容）
void PeerCore::proxy_push_received(Json req, SOCKET conn) {
    std::string pid = jstr(req, "proxy_id");
    if (pid.empty()) {
        Json reply = Json::obj();
        reply.set("cmd", Json("proxy_ok"));
        reply.set("proxy_id", Json(""));
        reply.set("ok", Json(false));
        reply.set("mac", Json(uid));
        send_json(conn, reply);
        return;
    }

    std::string kind = jstr_or(req, "kind", "file");
    std::string fid = jstr(req, "file_id");
    std::string sender_uid = jstr(req, "sender_uid");
    std::string sender_name = jstr_or(req, "sender_name", "?");
    std::string fname = jstr(req, "name");
    std::string gid = jstr(req, "gid");
    std::string gname = jstr(req, "gname");
    std::vector<std::string> targets = jstr_list(req, "targets");
    long long rsize = jint(req, "size", 0);
    std::string ip = jstr(req, "ip");
    long long port = jint(req, "port", 0);

    if (ip.empty() || port <= 0) {
        PeerInfo p;
        if (resolve_peer(*this, sender_uid, sender_name, p)) { ip = p.ip; port = p.tcp_port; }
    }

    if (kind == "file") {
        std::string dest = proxy_file_path(*this, pid);
        std::string err;
        if (!pull_proxy_file(*this, ip, (int)port, fid, pid, true, dest, err)) {
            Json reply = Json::obj();
            reply.set("cmd", Json("proxy_ok"));
            reply.set("proxy_id", Json(pid));
            reply.set("ok", Json(false));
            reply.set("err", Json("拉取代理文件失败"));
            reply.set("mac", Json(uid));
            send_json(conn, reply);
            return;
        }
    }

    double now = now_ts();

    // 原作者收到回传：自己本来就有原文件，不保留代理
    if (!sender_uid.empty() && sender_uid == uid) {
        remove_file(proxy_file_path(*this, pid));
        Json reply = Json::obj();
        reply.set("cmd", Json("proxy_ok"));
        reply.set("proxy_id", Json(pid));
        reply.set("ok", Json(true));
        reply.set("mac", Json(uid));
        send_json(conn, reply);
        return;
    }

    ProxyRec r;
    r.proxy_id = pid;
    r.kind = kind;
    r.gid = gid;
    r.gname = gname;
    r.file_id = fid;
    r.name = fname;
    r.size = rsize;
    r.text = jstr(req, "text");
    r.sender_uid = sender_uid;
    r.sender_name = sender_name;
    r.from_mac = sender_uid;
    {
        double rts = jget(req, "ts").asNum(0.0);   // Python: float(ts or now)
        if (rts <= 0.0) rts = now;
        r.created_ts = rts;
        r.ts = rts;
    }
    r.targets = targets;
    r.consumed = false;
    r.holder = "";
    proxy_store_local(r, sender_uid);

    Json reply = Json::obj();
    reply.set("cmd", Json("proxy_ok"));
    reply.set("proxy_id", Json(pid));
    reply.set("ok", Json(true));
    reply.set("mac", Json(uid));
    send_json(conn, reply);

    if (has_str(targets, uid)) {
        // 最终接受者（群成员本人）：登记进正常文件索引并广播 file_pub（design_v2 2.6）
        if (kind == "file" && !fid.empty()) {
            std::string prox = proxy_file_path(*this, pid);
            std::string cache_file = path_join(cache_dir(), fid);
            if (file_exists(prox) && !file_exists(cache_file)) copy_file(prox, cache_file, true);

            Json rr = Json::obj();
            rr.set("name", Json(fname));
            rr.set("size", Json(rsize));
            rr.set("ts", Json(now_ts()));
            rr.set("src_uid", Json(sender_uid));
            rr.set("src_name", Json(sender_name));
            Json gs = Json::arr();
            if (!gid.empty()) gs.push(Json(gid));
            rr.set("groups", gs);
            rr.set("has_local", Json(true));
            register_file(fid, rr);

            Json pub = Json::obj();
            pub.set("t", Json("file_pub"));
            pub.set("gid", Json(gid));
            pub.set("gname", Json(gname));
            Json fo = Json::obj();
            fo.set("id", Json(fid));
            fo.set("name", Json(fname));
            fo.set("size", Json(rsize));
            pub.set("file", fo);
            pub.set("ts", Json(now_ts()));
            pub.set("sender_uid", Json(sender_uid));
            pub.set("sender_name", Json(sender_name));
            pub.set("uid", Json(uid));
            pub.set("name", Json(my_name()));
            pub.set("msg_id", Json(new_msg_id(uid, ++msg_counter)));
            broadcast(pub);

            Json ev = Json::obj();
            ev.set("file", file_view(fid));
            ev.set("gid", Json(gid));
            ev.set("from_self", Json(false));
            emit("file_new", {ev});
            emit_act_log(*this, "已收到群「" + gname + "」的离线文件「" + fname +
                                    "」（来自设备数据传输代理）", now_ts());
        } else {
            emit_act_log(*this, "已收到群「" + gname + "」的离线信息（来自设备数据传输代理）", now_ts());
        }
    }
}

// ================================================================ 上线提醒 ==

void PeerCore::proxy_loop() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    while (running) {
        try {
            if (in_net()) {
                Json m = Json::obj();
                m.set("t", Json("proxy_online"));
                m.set("uid", Json(uid));
                m.set("name", Json(my_name()));
                m.set("msg_id", Json(new_msg_id(uid, ++msg_counter)));
                m.set("mac", Json(uid));
                Json gs = Json::arr();
                {
                    std::lock_guard<std::recursive_mutex> lk(mtx);
                    for (const auto& kv : groups) gs.push(Json(kv.first));
                }
                m.set("groups", gs);
                double now = now_ts();
                m.set("ts", Json(now));
                broadcast(m);
                proxy_last_online_bcast = now;
            }
        } catch (...) {
            // 与 Python _safe 一致：单次失败不影响循环
        }
        double iv = proxy_cfg().presence_interval * 4.0;
        if (iv < 1.0) iv = 1.0;
        // 按 200ms 切片睡眠，保证 stop() 能及时退出
        int slices = (int)(iv * 5.0);
        for (int i = 0; i < slices && running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

// ================================================================ 启动代理 ==

bool PeerCore::proxy_start(const std::string& kind_in, const std::string& gid,
                           const Json& file, const std::string& text_in) {
    if (!in_net()) return false;
    std::string kind = kind_in.empty() ? "file" : kind_in;

    std::string gname;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        auto it = groups.find(gid);
        if (it != groups.end()) gname = it->second.get("gname").asStr();
    }

    // 该群其他成员（group_members 里非 self 的 mac/uid）
    std::vector<std::string> targets;
    for (const Json& m : group_members(gid)) {
        if (m.get("self").asBool(false)) continue;
        std::string muid = m.get("uid").asStr();
        if (muid.empty() || muid == uid) continue;
        add_unique(targets, muid);
    }
    // 群内还有别的在线成员 -> 不走代理
    for (const auto& t : targets) {
        if (is_online(t)) return false;
    }

    std::string fid = jget(file, "id").asStr();
    std::string fname = jget(file, "name").asStr();
    long long fsize = jget(file, "size").asInt(0);

    std::string pid = to_lower_ascii(random_hex_upper(16));
    double now = now_ts();

    ProxyRec rec;
    rec.proxy_id = pid;
    rec.kind = kind;
    rec.gid = gid;
    rec.gname = gname;
    rec.file_id = fid;
    rec.name = fname;
    rec.size = fsize;
    rec.text = (kind == "msg") ? text_in : "";
    rec.sender_uid = uid;
    rec.sender_name = my_name();
    rec.from_mac = uid;
    rec.created_ts = now;
    rec.ts = now;
    rec.targets = targets;
    rec.consumed = false;
    rec.holder = "";
    rec.holders.push_back(uid);
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        proxies[pid] = rec;
    }
    no_target_add(this, pid);
    save_proxies();

    std::string req_id = new_msg_id(uid, ++msg_counter);
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        proxy_reqs[req_id] = Json::arr();
    }

    emit_act_log(*this, "「" + my_name() + "」发起在线请求：群「" + gname +
                            "」当前没有其他成员在线，向局域网内所有在线成员请求" +
                            (kind == "file" ? "文件" : "信息") + "代理", now);

    Json req = Json::obj();
    req.set("t", Json("proxy_req"));
    req.set("uid", Json(uid));
    req.set("name", Json(my_name()));
    req.set("msg_id", Json(new_msg_id(uid, ++msg_counter)));
    req.set("req_id", Json(req_id));
    req.set("gid", Json(gid));
    req.set("gname", Json(gname));
    req.set("kind", Json(kind));
    Json fo = Json::obj();
    fo.set("id", Json(fid));
    fo.set("name", Json(fname));
    fo.set("size", Json(fsize));
    req.set("file", fo);
    req.set("text", Json(rec.text));
    req.set("targets", str_array(targets));
    req.set("sender_uid", Json(uid));
    req.set("sender_name", Json(my_name()));
    req.set("net_id", Json(net_nid(*this)));
    req.set("ts", Json(now_ts()));
    broadcast(req);

    // 等 PROXY_REQ_WAIT 秒收集在线应答
    double deadline = now_ts() + proxy_cfg().proxy_req_wait;
    while (running && now_ts() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    Json acks = Json::arr();
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        auto it = proxy_reqs.find(req_id);
        if (it != proxy_reqs.end()) {
            acks = it->second;
            proxy_reqs.erase(it);
        }
    }

    if (!acks.isArr() || acks.items().empty()) {
        emit_act_log(*this, "局域网内没有其他在线成员可以代存，代理未建立（文件仍保留在本机）",
                     now_ts());
        return true;
    }

    for (const Json& a : acks.items()) {
        std::string amac = a.get("mac").asStr();
        std::string aname = jstr_or(a, "name", "?");
        std::string aip = a.get("ip").asStr();
        long long aport = a.get("port").asInt(0);
        if (aip.empty() || aport <= 0) {
            PeerInfo p;
            if (resolve_peer(*this, amac, aname, p)) { aip = p.ip; aport = p.tcp_port; }
        }
        PeerInfo peer;
        peer.uid = amac;
        peer.name = aname;
        peer.ip = aip;
        peer.tcp_port = (int)aport;

        if (!proxy_store_on(*this, peer, rec)) continue;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            auto it = proxies.find(pid);
            if (it != proxies.end()) add_unique(it->second.holders, amac);
        }
        save_proxies();
        if (kind == "file") {
            emit_act_log(*this, "已向「" + aname + "」发送文件代理「" + rec.name +
                                    "」（临时寄存，用户不可查看）", now_ts());
        } else {
            emit_act_log(*this, "已向「" + aname + "」发送信息代理", now_ts());
        }
    }
    return true;
}

// ================================================== 群聊：离线信息代理入口 ==

bool PeerCore::send_group_text_with_proxy(const std::string& gid, const std::string& text_in) {
    std::string text = trim(text_in);
    int maxlen = proxy_cfg().max_chat_len;
    if (maxlen > 0 && (int)text.size() > maxlen) text = utf8_prefix_keep(text, (size_t)maxlen);
    if (text.empty()) return false;

    std::string gname;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        auto it = groups.find(gid);
        if (it == groups.end()) return false;
        gname = it->second.get("gname").asStr();
    }
    if (!in_net()) return false;

    Json m = Json::obj();
    m.set("t", Json("chat"));
    m.set("gid", Json(gid));
    m.set("gname", Json(gname));
    m.set("text", Json(text));
    m.set("ts", Json(now_ts()));
    m.set("uid", Json(uid));
    m.set("name", Json(my_name()));
    m.set("msg_id", Json(new_msg_id(uid, ++msg_counter)));
    broadcast(m);

    record_activity("chat", my_name() + ": " + text);

    Json ev = Json::obj();
    ev.set("gid", Json(gid));
    ev.set("gname", Json(gname));
    ev.set("from_name", Json(my_name()));
    ev.set("from_uid", Json(uid));
    ev.set("text", Json(text));
    ev.set("ts", Json(now_ts()));
    ev.set("self", Json(true));
    emit("chat", {ev});

    // 群内没有任何其他在线成员 -> 交给数据代理，等成员上线后自动送达
    for (const Json& mm : group_members(gid)) {
        if (mm.get("self").asBool(false)) continue;
        std::string muid = mm.get("uid").asStr();
        if (muid.empty() || muid == uid) continue;
        if (is_online(muid)) return false;
    }
    return proxy_start("msg", gid, Json(), text);
}

} // namespace nw
