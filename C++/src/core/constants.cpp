// constants.cpp —— 全局常量、事件通道、配置读写、更新清单解析
#include "core.h"
#include "../common/util.h"
#include "../common/sha2.h"
#include "../common/crypto.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace nw {

// ---------------------------------------------------------------- 常量 --
const char* APP_NAME = "内部网";
const char* APP_VERSION_FALLBACK = "2.1.0";

const char* UPDATE_MANIFEST_NAME = "update.wenyi";
const char* MAIN_PROGRAM_NAME = "neiwang.exe";
const char* UPDATER1_PROGRAM_NAME = "neiwang_update1.exe";
const char* UPDATER2_PROGRAM_NAME = "neiwang_update2.exe";

const char* GITHUB_OWNER = "ZZZ-need-sleep";
const char* GITHUB_REPO = "Local_Network_Communication-created-by-DeepDeek-";
const char* GITHUB_BRANCH = "main";
const char* CUSTOMER_DATA_DIR = "customer_god";
const char* UPDATE_CLONE_DIR = "update_clone";

const char* NET_NO_MEMBER = "（不属于本内网）";

static std::string env_str(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

static int env_int(const char* name, int def) {
    std::string v = trim(env_str(name));
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

static double env_float(const char* name, double def) {
    std::string v = trim(env_str(name));
    if (v.empty()) return def;
    try { return std::stod(v); } catch (...) { return def; }
}

void load_env_overrides(Cfg& c) {
    c.udp_port = env_int("NEIWANG_UDP_PORT", c.udp_port);
    c.tcp_port = env_int("NEIWANG_TCP_PORT", c.tcp_port);
    c.presence_interval = env_float("NEIWANG_PRES_INTERVAL", c.presence_interval);
    c.presence_ttl      = env_float("NEIWANG_PRES_TTL", c.presence_ttl);
    c.offer_wait        = env_float("NEIWANG_OFFER_WAIT", c.offer_wait);
    c.rename_vote_need  = env_int("NEIWANG_VOTE_NEED", c.rename_vote_need);
    c.rename_vote_wait  = env_float("NEIWANG_VOTE_WAIT", c.rename_vote_wait);
    c.net_limit_no_vote = env_int("NEIWANG_NET_NO_VOTE", c.net_limit_no_vote);
    c.net_limit_full    = env_int("NEIWANG_NET_FULL_VOTE", c.net_limit_full);
    c.net_scan_window   = env_float("NEIWANG_SCAN_WINDOW", c.net_scan_window);
    c.net_vote_window   = env_float("NEIWANG_NET_VOTE_WINDOW", c.net_vote_window);
    c.net_vote_change   = env_float("NEIWANG_NET_VOTE_CHANGE", c.net_vote_change);
    c.net_announce_interval = env_float("NEIWANG_NET_ANNOUNCE", c.net_announce_interval);
    c.net_req_rebroadcast   = env_float("NEIWANG_NET_REQ_REBROADCAST", c.net_req_rebroadcast);
    c.net_nearby_ttl        = env_float("NEIWANG_NET_NEARBY_TTL", c.net_nearby_ttl);
    c.act_hello_interval = env_float("NEIWANG_ACT_HELLO", c.act_hello_interval);
    c.act_pull_wait      = env_float("NEIWANG_ACT_WAIT", c.act_pull_wait);
    c.proxy_req_wait     = env_float("NEIWANG_PROXY_WAIT", c.proxy_req_wait);
    c.proxy_group_wait   = env_float("NEIWANG_PROXY_GROUP_WAIT", c.proxy_group_wait);
}

// now_ts() 由 common/util.cpp 提供（util.h 与 core.h 是同一符号）

std::string new_msg_id(const std::string& uid, long long counter) {
    char buf[160];
    std::string rnd = random_hex_upper(3);
    for (auto& ch : rnd) ch = (char)tolower((unsigned char)ch);
    snprintf(buf, sizeof(buf), "%s-%lld-%s", uid.c_str(), counter, rnd.c_str());
    return std::string(buf);
}

std::pair<int, std::string> net_vote_need(int members, const Cfg& cfg) {
    char buf[320];
    if (members < 0) members = 0;
    if (members <= cfg.net_limit_no_vote) {
        snprintf(buf, sizeof(buf), "内网 %d 人（不超过 %d 人），免投票自动通过",
                 members, cfg.net_limit_no_vote);
        return {0, buf};
    }
    if (members <= cfg.net_limit_full) {
        snprintf(buf, sizeof(buf), "内网 %d 人（%d–%d 人），需全票通过（%d 票）",
                 members, cfg.net_limit_no_vote + 1, cfg.net_limit_full, members);
        return {members, buf};
    }
    int need = members / 3 + 1;
    snprintf(buf, sizeof(buf), "内网 %d 人（%d 人及以上），需超过 1/3 同意即 %d 票",
             members, cfg.net_limit_full + 1, need);
    return {need, buf};
}

// ------------------------------------------------------------ 事件通道 --
void EventQueue::push(const Event& e) {
    {
        std::lock_guard<std::mutex> g(m_);
        q_.push_back(e);
        if (q_.size() > 20000) q_.pop_front();
    }
    cv_.notify_one();
}

bool EventQueue::pop(Event& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [this] { return !q_.empty(); }))
        return false;
    out = q_.front();
    q_.pop_front();
    return true;
}

void EventQueue::clear() {
    std::lock_guard<std::mutex> g(m_);
    q_.clear();
}

size_t EventQueue::size() {
    std::lock_guard<std::mutex> g(m_);
    return q_.size();
}

// ------------------------------------------------------------ 清单/版本 --
Json parse_update_manifest(const std::string& data_in) {
    Json out = Json::obj();
    out.set("version", Json(""));
    out.set("notes", Json(""));
    std::string data = trim(data_in);
    if (data.empty()) return out;
    // 先尝试 JSON
    bool ok = false;
    Json obj = Json::parse(data, &ok);
    if (ok && obj.isObj() && obj.has("version")) {
        std::string ver = trim(obj.get("version").asStr(""));
        Json notes = obj.get("notes");
        if (notes.isNull()) notes = obj.get("content");
        if (notes.isNull()) notes = obj.get("changelog");
        std::string ns;
        if (notes.isArr()) {
            std::vector<std::string> parts;
            for (auto& x : notes.items()) parts.push_back(x.isStr() ? x.asStr() : x.dump());
            ns = join(parts, "\n");
        } else if (!notes.isNull()) {
            ns = notes.isStr() ? notes.asStr("") : notes.dump();
            ns = trim(ns);
        }
        out.set("version", Json(ver));
        out.set("notes", Json(ns));
        return out;
    }
    // 纯文本
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char ch : data) {
            if (ch == '\n') { lines.push_back(cur); cur.clear(); }
            else if (ch != '\r') cur.push_back(ch);
        }
        lines.push_back(cur);
    }
    std::string ver = lines.empty() ? "" : trim(lines[0]);
    std::string ns;
    for (size_t i = 1; i < lines.size(); ++i) {
        if (i > 1) ns += "\n";
        ns += lines[i];
    }
    out.set("version", Json(ver));
    out.set("notes", Json(trim(ns)));
    return out;
}

std::vector<long long> version_key(const std::string& ver) {
    std::vector<long long> out;
    std::string cur;
    for (size_t i = 0; i <= ver.size(); ++i) {
        char ch = (i < ver.size()) ? ver[i] : '\0';
        if (ch >= '0' && ch <= '9') cur.push_back(ch);
        else {
            if (!cur.empty()) { try { out.push_back(std::stoll(cur)); } catch (...) {} cur.clear(); }
        }
    }
    if (out.empty()) out.push_back(0);
    return out;
}

bool is_newer_version(const std::string& remote, const std::string& local) {
    std::vector<long long> r = version_key(remote), l = version_key(local);
    size_t n = (std::max)(r.size(), l.size());
    r.resize(n, 0);
    l.resize(n, 0);
    return r > l;
}

// 本版在仓库里的目录名（仓库根下有 C++/ 与 Python/ 两个版本，各自更新自己）
const char* VER_DIR_NAME = "C++";

// ---------------------------------------------------------------- 配置 --
static std::string default_update_url() {
    return std::string("https://raw.githubusercontent.com/") + GITHUB_OWNER + "/" +
           GITHUB_REPO + "/" + GITHUB_BRANCH + "/" + VER_DIR_NAME + "/" + UPDATE_MANIFEST_NAME;
}
static std::string default_update_repo() {
    return std::string("https://github.com/") + GITHUB_OWNER + "/" + GITHUB_REPO + ".git";
}
static std::string default_update_page() {
    return std::string("https://github.com/") + GITHUB_OWNER + "/" + GITHUB_REPO;
}

Json load_config(const std::string& data_dir, const Cfg& cfg, const std::string& /*exe_dir*/) {
    Json c = Json::obj();
    std::string env_name = env_str("NEIWANG_NAME");
    c.set("name", Json(env_name.empty() ? hostname_utf8() : env_name));
    c.set("uid", Json("PENDING"));
    c.set("tcp_port", Json((long long)cfg.tcp_port));
    c.set("udp_port", Json((long long)cfg.udp_port));
    c.set("groups", Json::arr());
    c.set("update_url", Json(default_update_url()));
    c.set("update_page", Json(default_update_page()));
    c.set("update_repo", Json(default_update_repo()));
    c.set("update_package", Json(""));
    c.set("saved_files", Json::arr());
    c.set("rename_count", Json((long long)0));
    c.set("rename_vote_need", Json((long long)cfg.rename_vote_need));

    std::string path = path_join(data_dir, "config.json");
    std::string text;
    if (file_exists(path) && read_file_utf8(path, text)) {
        bool ok = false;
        Json d = Json::parse(text, &ok);
        if (ok && d.isObj()) {
            for (auto& k : d.keys()) c.set(k, d.get(k));
        }
    }
    if (!c.has("uid") || c.get("uid").asStr("") == "PENDING" || c.get("uid").asStr("").empty())
        c.set("uid", Json(machine_uid()));
    if (c.get("name").asStr("").empty()) c.set("name", Json(hostname_utf8()));
    if (!c.has("tcp_port") || c.get("tcp_port").isNull()) c.set("tcp_port", Json((long long)cfg.tcp_port));
    if (!c.has("udp_port") || c.get("udp_port").isNull()) c.set("udp_port", Json((long long)cfg.udp_port));
    if (!c.has("groups") || !c.get("groups").isArr()) c.set("groups", Json::arr());
    if (c.get("update_url").asStr("").empty()) c.set("update_url", Json(default_update_url()));
    if (c.get("update_page").asStr("").empty()) c.set("update_page", Json(default_update_page()));
    if (c.get("update_repo").asStr("").empty()) c.set("update_repo", Json(default_update_repo()));
    if (!c.has("update_package")) c.set("update_package", Json(""));
    if (!c.has("rename_count")) c.set("rename_count", Json((long long)0));
    if (!c.has("rename_vote_need")) c.set("rename_vote_need", Json((long long)cfg.rename_vote_need));
    save_config(data_dir, c);
    return c;
}

void save_config(const std::string& data_dir, const Json& cfg) {
    ensure_dir(data_dir);
    write_file_utf8(path_join(data_dir, "config.json"), cfg.dump(true));
}

} // namespace nw
