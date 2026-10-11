// core.h —— 「内部网 局域网群组」C++ 版核心：常量、事件通道、PeerCore 声明
// 与 Python 版 neiwang.py 的 PeerCore 一一对应，协议完全兼容。
// 本文件是所有 core/*.cpp 与 ui/*.cpp 的公共契约，修改它必须同步 docs/design_v2.md。
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../common/json.h"
#include "netio.h"

namespace nw {

// ---------------------------------------------------------------- 版本/名称 --
extern const char* APP_NAME;          // "内部网"
extern const char* APP_VERSION_FALLBACK;  // "2.0.0"

extern const char* UPDATE_MANIFEST_NAME;  // "update.wenyi"
extern const char* MAIN_PROGRAM_NAME;     // "neiwang.exe"
extern const char* UPDATER1_PROGRAM_NAME; // "neiwang_update1.exe"
extern const char* UPDATER2_PROGRAM_NAME; // "neiwang_update2.exe"

extern const char* GITHUB_OWNER;
extern const char* GITHUB_REPO;
extern const char* GITHUB_BRANCH;
extern const char* CUSTOMER_DATA_DIR;   // "customer_god"
extern const char* UPDATE_CLONE_DIR;    // "update_clone"

#define NW_OS_WINDOWS 1

// ---------------------------------------------------------------- 事件通道 --
// Python 的 _emit(kind, *args) 在 C++ 里表示为 Event{kind, args(数组)}，语义完全一致。
struct Event {
    std::string kind;
    Json args;   // 数组
    Event() : args(Json::arr()) {}
    Event(const std::string& k, Json a) : kind(k), args(a) {}
};

class EventQueue {
public:
    void push(const Event& e);
    // 阻塞等待最多 timeout_ms 毫秒；取到返回 true
    bool pop(Event& out, int timeout_ms);
    void clear();
    size_t size();
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Event> q_;
};

// ---------------------------------------------------------------- 常量 --
struct Cfg {
    int    udp_port   = 45455;
    int    tcp_port   = 45431;

    double presence_interval = 8.0;
    double presence_ttl      = 30.0;
    double offer_wait        = 8.0;
    int    max_chat_len      = 4000;
    int    file_chunk        = 128 * 1024;
    int    log_max_lines     = 800;
    double seen_ttl          = 120.0;
    int    fallback_retry    = 2;

    int    rename_vote_need  = 5;
    double rename_vote_wait  = 120.0;

    int    secret_relay_retry = 3;

    int    net_limit_no_vote = 5;
    int    net_limit_full    = 7;
    double net_scan_window   = 3.0;
    double net_vote_window   = 86400.0;
    double net_vote_change   = 300.0;
    double net_announce_interval = 15.0;
    double net_req_rebroadcast   = 10.0;
    double net_nearby_ttl        = 40.0;

    // ---- v2.0.0 新增 ----
    double act_hello_interval = 60.0;   // 活动数据问询间隔
    double act_have_throttle  = 0.5;    // 应答节流
    double act_pull_wait      = 6.0;    // 补齐等待窗口
    int    act_max_events     = 5000;
    int    act_send_limit     = 2000;

    double proxy_req_wait     = 10.0;
    double proxy_group_wait   = 300.0;
    double proxy_push_timeout = 15.0;
};

extern const char* NET_NO_MEMBER;   // "（不属于本内网）"

// 环境变量覆盖（与 Python 的 NEIWANG_* 完全一致）
void load_env_overrides(Cfg& c);

// ---------------------------------------------------------------- 工具 --
double now_ts();
std::string new_msg_id(const std::string& uid, long long counter);

// 内网准入投票规则：返回 (需要的同意票数, 规则说明)
std::pair<int, std::string> net_vote_need(int members, const Cfg& cfg);

// 更新清单解析 / 版本比较
Json parse_update_manifest(const std::string& data);   // {"version":..., "notes":...}
std::vector<long long> version_key(const std::string& ver);
bool is_newer_version(const std::string& remote, const std::string& local);

// ---------------------------------------------------------------- 配置 --
// 与 Python load_config() 等价：读 data/config.json，缺失字段补默认，写回。
Json load_config(const std::string& data_dir, const Cfg& cfg, const std::string& exe_dir);
void save_config(const std::string& data_dir, const Json& cfg);

// ---------------------------------------------------------------- 核心 --
struct PeerInfo {
    std::string uid, name, ip;
    int tcp_port = 0;
    Json groups = Json::arr();
    std::string ver;
    double ts = 0.0;
};

struct NetMember {
    std::string mac, name, ip;
    int tcp_port = 0;
    double joined_ts = 0.0;
    bool founder = false;
};

// 活动历史事件（design_v2 第 1.1 节）
struct ActEvent {
    std::string eid;
    long long seq = 0;
    double ts = 0.0;
    std::string uid, name, kind, text;
};

// 数据代理记录（design_v2 第 2.1 节）
struct ProxyRec {
    std::string proxy_id, kind, gid, gname, file_id, name, text;
    long long size = 0;
    std::string sender_uid, sender_name, from_mac;
    double created_ts = 0.0, ts = 0.0;
    std::vector<std::string> targets, delivered_to, holders;
    bool consumed = false;
    std::string holder;         // JSON 字段 "holder"：接受者 mac，无人保留时为空串
};

// 下载任务
struct DlTask {
    std::string fid, name, gid, gname, sender_uid, sender_name;
    long long size = 0;
    int attempt = 0;
};

class PeerCore {
public:
    // ---- 构造/生命周期 -------------------------------------------------
    PeerCore(Json cfg, EventQueue* events,
             std::function<std::string()> name_getter,
             std::string data_root, std::string exe_dir);
    ~PeerCore();

    void start();
    void stop();

    // ---- 基本属性 ------------------------------------------------------
    std::string my_name() const;
    std::vector<Json> my_groups() const;
    int my_tcp_port() const;
    bool in_net() const;
    void require_net();

    // ---- 查询（供 GUI） ------------------------------------------------
    std::vector<Json> group_members(const std::string& gid);
    bool is_online(const std::string& uid);
    std::vector<Json> files_of_group(const std::string& gid);
    Json file_view(const std::string& fid);
    Json net_view();
    std::vector<Json> net_nearby_list();
    std::vector<Json> net_pending_for_me();
    std::vector<Json> net_open_votes();
    std::vector<Json> net_vote_history();
    Json peer_list();
    std::vector<std::string> log_lines();

    // ---- 群组 ----------------------------------------------------------
    Json create_group(const std::string& gname);
    Json join_group(const std::string& gid, const std::string& gname);
    void leave_group(const std::string& gid);
    void send_chat(const std::string& gid, const std::string& text);

    // ---- 私聊 / 转存 ---------------------------------------------------
    bool send_dm(const std::string& to_uid, const std::string& text);
    Json send_dm_auto(const std::string& to_uid, const std::string& text);

    // ---- 改名投票 ------------------------------------------------------
    void rename_self(const std::string& new_name);
    std::string start_rename_vote(const std::string& new_name);
    void cast_rename_vote(const std::string& pid, bool approve);

    // ---- 文件 ----------------------------------------------------------
    void send_file_to_group(const std::string& file_path, const std::string& gid);

    // ---- 内网 ----------------------------------------------------------
    Json net_create(const std::string& name);
    void net_leave();
    void net_rename(const std::string& name);
    std::vector<Json> net_scan(double seconds);
    Json net_apply_join(const std::string& nid);
    void net_cancel_apply();
    void net_cast_vote(const std::string& req_id, int approve /* -1 弃权,0 拒绝,1 同意 */);

    // ---- 更新检查 ------------------------------------------------------
    void check_update(bool manual);

    // ---- v2.0.0 新增：活动历史 ----------------------------------------
    bool has_activity_data() const;
    double activity_last_get_ts() const;
    void record_activity(const std::string& kind, const std::string& text, double ts = 0.0);
    void merge_activity(const std::vector<ActEvent>& events);
    Json activity_view();                       // 给 GUI 状态条用

    // ---- v2.0.0 新增：设备数据传输代理 ---------------------------------
    Json proxy_list();                          // 仅测试/调试用，GUI 不展示
    bool proxy_start(const std::string& kind, const std::string& gid,
                     const Json& file, const std::string& text);
    bool proxy_delete(const std::string& proxy_id, const std::string& holder);

    // ---- 空设备判定（design_v2 第 3 节） -------------------------------
    bool is_blank_device() const;
    // 进程启动那一刻的「空设备」快照：用于避免自动组网（net_create）之后判定失效
    bool was_blank_at_start() const { return blank_at_start; }
    bool blank_at_start = false;

    // ================= 以下是内部状态（与 Python 同名），core/*.cpp 直接用 ==
    Json cfg;
    std::string uid;
    EventQueue* events = nullptr;
    std::function<std::string()> name_getter;
    std::string data_root, exe_dir;

    std::map<std::string, Json> groups;              // gid -> {gid,gname}
    std::map<std::string, PeerInfo> peers;           // uid -> PeerInfo
    std::map<std::string, Json> discovered;          // gid -> {...}
    std::map<std::string, double> seen;              // msg_id -> ts
    std::atomic<long long> msg_counter{0};

    std::map<std::string, Json> index;               // file_id -> {...}
    std::map<std::string, Json> secrets;             // store_id -> {...}
    std::set<std::string> recv_secrets;
    std::set<std::string> delivering;

    std::map<std::string, std::vector<Json>> offers; // req_id -> [(ip,port,uid)]
    std::set<std::string> downloading;
    std::deque<DlTask> download_queue;
    std::map<std::string, Json> active_downloads;

    std::atomic<bool> running{true};
    std::recursive_mutex mtx;
    SOCKET udp_sock = INVALID_SOCKET;
    SOCKET tcp_srv  = INVALID_SOCKET;
    std::vector<std::thread> threads;

    std::vector<Json> file_offers;

    // 改名投票
    std::map<std::string, Json> rename_votes;
    std::set<std::string> rename_requests;
    std::set<std::string> my_votes;

    // 内网
    Json net;                                        // null 或 {...}
    std::map<std::string, Json> net_members;         // mac -> {mac,name,ip,tcp_port,joined_ts,founder}
    std::map<std::string, Json> net_reqs;
    std::map<std::string, Json> net_nearby;
    std::map<std::string, Json> net_my_votes;
    Json net_applying;                               // null 或 {...}
    double net_last_announce = 0, net_last_sync = 0, net_last_req = 0;
    double net_last_vote_replay = 0, net_reply_ts = 0;
    bool net_suppress_auto = false;

    // 公共日志（GUI 只读）
    std::deque<std::string> logs;
    // 在线提醒去重：记录已知在线的 uid
    std::set<std::string> online_known;

    // ---- v2.0.0 活动历史状态 ----
    std::vector<ActEvent> act_events;
    long long act_seq = 0;
    double act_first_ts = 0, act_last_get_ts = 0;
    double act_last_hello = 0, act_last_save = 0;
    std::string act_net_id;
    // 正在进行的补齐：收集应答
    std::map<std::string, Json> act_haves;           // mac -> act_have
    double act_pull_deadline = 0;
    bool act_pulling = false;
    std::string act_run_since_str;
    double act_run_since = 0;

    // ---- v2.0.0 代理状态 ----
    std::map<std::string, ProxyRec> proxies;
    std::map<std::string, Json> proxy_reqs;          // req_id -> 收集到的 ack
    double proxy_last_online_bcast = 0;

    // ================= 内部方法（按 .cpp 文件归属） =====================
    // ---- peercore.cpp ----
    void emit(const std::string& kind, std::initializer_list<Json> args);
    void emit_log(const std::string& line, double ts = 0.0);
    void broadcast(Json msg, const std::string& only_ip = "");
    void udp_loop();
    void tcp_loop();
    void presence_loop();
    void sweep_loop();
    void download_worker();
    void dispatch_udp(Json msg, const std::string& from_ip);
    void prune_seen();
    void on_presence(Json msg, const std::string& from_ip);
    void on_group_announce(Json msg);
    void on_chat(Json msg);
    void on_dm(Json msg);
    void on_log(Json msg);
    void on_rename_req(Json msg);
    void on_rename_vote(Json msg);
    void broadcast_log(const std::string& text);
    void persist_groups();
    void apply_rename(const std::string& new_name, const std::string& old_name,
                      const std::string& note);
    std::string my_gid_gname(const std::string& gid, const std::string& fallback);

    // ---- peercore_net.cpp ----
    std::string net_path();
    void load_net();
    void save_net();
    bool member_ok(const std::string& uid);
    void net_announce(const std::string& only_ip = "");
    void net_sync(const std::string& only_ip = "");
    void net_join_req_broadcast();
    bool net_add_member(const std::string& mac, const std::string& name, const std::string& ip,
                        int tcp_port, bool founder);
    void on_net_scan(Json msg, const std::string& from_ip);
    void on_net_announce(Json msg, const std::string& from_ip);
    void on_net_join_req(Json msg, const std::string& from_ip);
    void on_net_join_vote(Json msg);
    void on_net_registry(Json msg, const std::string& from_ip);
    void on_net_member_left(Json msg);
    void on_net_join_result(Json msg);
    bool merge_req(Json remote);
    Json req_card(const std::string& rid, Json req);
    Json req_progress(Json req);
    void net_eval_req(Json& req);
    void net_approve_req(Json& req, const std::string& note);
    void net_reject_req(Json& req, const std::string& note);
    void net_join_success(Json registry);
    void net_loop();
    void net_bootstrap();
    void net_tick();
    void net_prune();
    void net_rescan_after_leave();

    // ---- peercore_file.cpp ----
    std::string cache_dir();
    std::string downloads_dir(const std::string& gid, const std::string& gname);
    std::string index_path();
    void load_index();
    void save_index();
    void load_secrets();
    void save_secrets();
    void load_recv_secrets();
    void save_recv_secrets();
    bool has_file(const std::string& file_id);
    Json index_record(const std::string& file_id);
    void register_file(const std::string& fid, Json rec);
    void on_file_pub(Json msg);
    void sync_req(const std::string& gid);
    void on_sync_req(Json msg);
    void do_download(DlTask task, int attempt);
    void public_request(DlTask task, const std::string& req_id);
    void on_file_req(Json msg, const std::string& from_ip);
    void on_file_have(Json msg);
    bool fetch_from(const std::string& ip, int port, const std::string& fid, const DlTask& task);
    void finish_download(const DlTask& task);
    void handle_conn(SOCKET conn, const std::string& peer_ip);
    // relay
    void relay_dm_worker(const std::string& to_uid, const std::string& text);
    bool store_secret_on(const PeerInfo& peer, const std::string& store_id, Json rec);
    void on_presence_hook_deliver(const std::string& uid);
    void deliver_secrets_to(const std::string& uid);
    bool deliver_one(const PeerInfo& peer, const std::string& store_id, Json rec);
    void on_secret_store(Json req);
    bool on_secret_deliver(Json req);

    // ---- peercore_activity.cpp (v2.0.0) ----
    void load_activity();
    void save_activity(bool force);
    void act_loop();
    void act_hello_broadcast();
    void on_act_hello(Json msg, const std::string& from_ip);
    void on_act_have(Json msg);
    void act_try_pull();
    bool act_fetch_from(const std::string& ip, int port, double since, std::vector<ActEvent>& out);
    void act_on_get(Json req, SOCKET conn);
    void act_apply_consumed_text(const std::string& text);
    Json act_event_to_json(const ActEvent& e);
    ActEvent act_event_from_json(const Json& j);

    // ---- peercore_proxy.cpp (v2.0.0) ----
    std::string proxy_dir();
    std::string proxy_path();
    void load_proxies();
    void save_proxies();
    void on_proxy_req(Json msg, const std::string& from_ip);
    void on_proxy_ack(Json msg);
    void on_proxy_online(Json msg);
    void on_proxy_consumed(Json msg);
    void proxy_on_peer_online(const std::string& mac, const std::string& uid,
                              const std::string& name, const std::vector<std::string>& groups);
    bool proxy_push_to(const PeerInfo& peer, const ProxyRec& rec, bool is_target);
    void proxy_store_received(Json req, SOCKET conn);   // TCP proxy_store（自己发应答帧）
    void proxy_push_received(Json req, SOCKET conn);    // TCP proxy_push（自己发应答帧）
    bool proxy_store_local(const ProxyRec& rec, const std::string& from_mac);
    void proxy_loop();
    bool send_group_text_with_proxy(const std::string& gid, const std::string& text);
};

} // namespace nw
