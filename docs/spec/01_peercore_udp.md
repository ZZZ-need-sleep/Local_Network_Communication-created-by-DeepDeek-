# 01 · PeerCore 协议规格（UDP 广播 / 内网准入 / TCP 帧）

> 本文是 `neiwang.py` 中 `class PeerCore` 的**逐字复刻级协议规格**，供「Python → C++」移植使用。
> 一切以源码为唯一依据；源码没有写的，一律显式标注 **「源码未定义」**。
> 本文**不包含**文件传输 / 下载调度主体的算法细节（仅在第 3 章登记其 type 字面量，便于清单完整）。

---

## 0. 源码基准与覆盖范围

| 项 | 值 |
| --- | --- |
| 源文件 | `E:\APP\DeepSeek-harness\工作区\mctire魔改\neiwang.py`（共 4760 行） |
| 类声明 | 第 567 行 `class PeerCore:` |
| `__init__` | 第 568–623 行 |
| **本文覆盖** | **第 567–2035 行**（`class PeerCore` 起，到「文件系统」小节之前；第 2036 行 `# ---- 文件系统 --` 起为文件传输部分，不在本文范围） |
| 附带引用的源码 | 第 459–482 行（TCP 帧编解码函数）、第 2396–2440 行（TCP 连接处理 `_tcp_loop` / `_handle_conn`，第 4 章需要）、第 3732–3792 行（GUI 侧 `_poll_events`，第 6 章事件需要） |
| 涉及的数据文件 | `data/config.json`、`data/net.json`、`data/index.json`、`data/secret_relay.json`、`data/secret_received.json` |
| 编码约定 | 所有 JSON 用 `json.dumps(obj, ensure_ascii=False)` → UTF-8 字节 |

`PeerCore` 构造签名（第 568 行）：

```python
PeerCore(cfg: dict, events: queue.Queue, name_getter=None, data_root: Path = None)
```

| 参数 | 类型 | 含义 | 默认 |
| --- | --- | --- | --- |
| `cfg` | dict | 配置（来自 `load_config()`） | 必填 |
| `events` | `queue.Queue` | 向 GUI 推事件的队列 | 必填 |
| `name_getter` | callable→str | 取当前昵称；缺省用 `lambda: cfg["name"]` | `None` |
| `data_root` | Path | 数据根目录；缺省 `DATA_DIR`（`程序目录/data`，可被环境变量 `NEIWANG_DATA_DIR` 覆盖） | `None` |

---

## 1. 全局常量（第 101–135 行）

所有可通过环境变量覆盖的常量，格式为 `int/float(os.environ.get(ENV, 默认值))`。

### 1.1 端口与心跳

| 常量 | 默认值 | 环境变量 | 含义 |
| --- | --- | --- | --- |
| `DEFAULT_UDP_PORT` | `45455` | `NEIWANG_UDP_PORT` | UDP 广播/接收端口 |
| `DEFAULT_TCP_PORT` | `45431` | `NEIWANG_TCP_PORT` | TCP 服务起始端口 |
| `PRESENCE_INTERVAL` | `8` 秒 | `NEIWANG_PRES_INTERVAL` | presence 心跳广播间隔 |
| `PRESENCE_TTL` | `30` 秒 | `NEIWANG_PRES_TTL` | 超过此值未收到 presence 视为离线 |
| `OFFER_WAIT` | `8` 秒 | `NEIWANG_OFFER_WAIT` | 公共询问等待应答秒数（文件部分） |
| `MAX_CHAT_LEN` | `4000` | 无 | 聊天/私聊文本最大字符数 |
| `FILE_CHUNK` | `131072`（128 KiB） | 无 | 文件分块大小、SHA-256 分块大小 |
| `LOG_MAX_LINES` | `800` | 无 | GUI 日志控件最多保留行数（GUI 侧使用，PeerCore 不用） |
| `SEEN_TTL` | `120` 秒 | 无 | **UDP 去重窗口** |
| `FALLBACK_RETRY` | `2` | 无 | 直连失败后回退公共询问的次数上限（文件部分） |
| `SECRET_RELAY_RETRY` | `3` | 无 | 保密转存送达失败重试上限 |

### 1.2 改名投票

| 常量 | 默认值 | 环境变量 | 含义 |
| --- | --- | --- | --- |
| `RENAME_VOTE_NEED` | `5` | `NEIWANG_VOTE_NEED` | 之后改名需**超过**的同意票数 |
| `RENAME_VOTE_WAIT` | `120` 秒 | `NEIWANG_VOTE_WAIT` | 改名投票等待时长，超时未过即失败 |

### 1.3 内网（准入）

| 常量 | 默认值 | 环境变量 | 含义 |
| --- | --- | --- | --- |
| `NET_LIMIT_NO_VOTE` | `5` | `NEIWANG_NET_NO_VOTE` | 成员数 ≤ 该值 → 免投票 |
| `NET_LIMIT_FULL` | `7` | `NEIWANG_NET_FULL_VOTE` | 成员数 ≤ 该值（且 > 上一值）→ 全票通过 |
| `NET_SCAN_WINDOW` | `3` 秒 | `NEIWANG_SCAN_WINDOW` | 启动扫描附近内网的时长 |
| `NET_VOTE_WINDOW` | `86400` 秒（1 天） | `NEIWANG_NET_VOTE_WINDOW` | 入网投票有效期 |
| `NET_VOTE_CHANGE` | `300` 秒（5 分钟） | `NEIWANG_NET_VOTE_CHANGE` | 投出后可改票的秒数 |
| `NET_ANNOUNCE_INTERVAL` | `15` 秒 | `NEIWANG_NET_ANNOUNCE` | 内网公告 + 登记表同步间隔 |
| `NET_REQ_REBROADCAST` | `10` 秒 | `NEIWANG_NET_REQ_REBROADCAST` | 入网申请重播间隔 |
| `NET_NEARBY_TTL` | `40` 秒 | `NEIWANG_NET_NEARBY_TTL` | 附近内网信息有效期 |
| `NET_NO_MEMBER` | `"（不属于本内网）"` | 无 | **源码中定义但从未被使用**（第 135 行）。移植时可忽略 |

### 1.4 其它硬编码数值（非常量，散布在逻辑中）

| 数值 | 位置 | 含义 |
| --- | --- | --- |
| `2.0` 秒 | `_net_loop` 第 1951 行 | 内网 tick 周期（`_net_tick` 每 2 秒执行一次） |
| `0.4` 秒 | `_net_loop` 第 1941 行 | 启动前等待 UDP 线程就绪 |
| `3` 秒 | `_sweep_loop` 第 880 行 | 在线表清扫周期 |
| `1.0` 秒 | `net_scan` 第 1514 行 | 扫描广播间隔 |
| `0.3` 秒 | `_rename_vote_waiter` 第 1214 行 | 投票等待轮询周期 |
| `60` 秒 | `_net_tick` 第 1997 行 | 我的投票重播周期 |
| `3600` 秒 | `_net_sync` 第 1606 行 | 已结束申请在登记表里保留时长 |
| `7 * 86400` 秒 | `_net_prune` 第 2027 行 | 已结束申请本地保留时长 |
| `4000` | `_dispatch_udp` 第 766 行 | `_seen` 字典长度上限 |
| `2000` | `_on_secret_deliver` 第 1085 行 | `_recv_secrets` 集合长度上限 |
| `1000` | `cast_rename_vote` 第 1239 行、`_on_rename_req` 第 1252 行 | `_my_votes` / `_rename_requests` 上限（**超过即整体清空**，不是丢最旧） |
| `64` | `_net_announce` 第 1596 行 | 公告里 `macs` 最多 64 个 |
| `30` | `_net_sync` 第 1607 行 | 登记表里 `reqs` 最多最后 30 条 |
| `0.5` 秒 | `_on_net_scan` 第 1657 行 | 扫描应答节流 |
| `6` / `8` / `8` / `60` / `30` 秒 | 见第 4 章 | 各 TCP 超时 |
| `65536` | `_udp_loop` 第 744 行 | UDP `recvfrom` 缓冲区 |
| `64 * 1024 * 1024` | `recv_json` 第 480 行 | TCP 帧长度上限 |

---

## 2. UDP 传输层通用契约

### 2.1 Socket 建立（`_make_udp`，第 679–688 行）

```
AF_INET, SOCK_DGRAM
setsockopt(SOL_SOCKET, SO_REUSEADDR, 1)
setsockopt(SOL_SOCKET, SO_BROADCAST, 1)
bind(("", cfg["udp_port"]))            # 失败(OSError) → bind(("", 0)) 由系统随机分配
settimeout(1.0)
```

> 注意：bind 失败回退到随机端口后，**不会**回写 `cfg["udp_port"]`，因此本机广播发出的目标端口始终是 `cfg["udp_port"]`（可能与实际监听端口不一致）。此为源码行为，移植时保持一致或标注差异。

### 2.2 接收循环（`_udp_loop`，第 741–757 行）

```
while self._running:
    try: data, addr = udp_sock.recvfrom(65536)
    except timeout: continue
    except OSError: break                  # socket 被 stop() 关闭
    try: msg = json.loads(data.decode("utf-8"))
    except: continue                       # 解析失败静默丢弃
    if not isinstance(msg, dict): continue
    if msg.get("uid") == self.uid: continue      # 自己的广播直接忽略
    self._dispatch_udp(msg, addr)
```

### 2.3 去重与分发（`_dispatch_udp`，第 759–804 行）

```
kind = msg.get("t"); mid = msg.get("msg_id")
if mid:                                    # 注意：只在 msg_id 非空时去重
    if mid in self._seen: return
    self._seen[mid] = now_ts()
    if len(self._seen) > 4000: self._prune_seen()
# kind → 处理函数（见第 3 章）
```

`_prune_seen`（第 806–809 行）：删除 `_seen` 中 `now_ts() - ts > SEEN_TTL(120)` 的条目。
**去重窗口 = SEEN_TTL = 120 秒**，且只在字典长度 > 4000 时才触发清理。

### 2.4 广播发送（`_broadcast`，第 712–734 行）

```
msg.setdefault("uid",    self.uid)         # 公共信封
msg.setdefault("name",   self.my_name)
msg.setdefault("msg_id", self._new_msg_id())
data = json.dumps(msg, ensure_ascii=False).encode("utf-8")

若 addr 非空：udp_sock.sendto(data, addr)   # 单播定向应答
否则：
  targets = {"255.255.255.255:udp"}
  for ip in get_local_ips():
      if ip.count(".") == 3: targets.add(ip.rsplit(".",1)[0] + ".255")
      targets.add(ip + ":udp")
  for t in targets:      # ":udp" 只是去重标记，发送前 replace(":udp","")
      try: udp_sock.sendto(data, (host, cfg["udp_port"]))
      except OSError: 忽略
异常时（仅当 self._running）打印堆栈
```

- **公共信封字段**：所有由 `_broadcast` 构造的报文都至少含 `uid`（发送者 MAC/设备 ID）、`name`（发送者昵称）、`msg_id`（去重 ID）。若报文本身已带同名字段则 `setdefault` 不覆盖。
- `_new_msg_id()`（第 736–738 行）：`f"{self.uid}-{self._msg_counter}-{secrets.token_hex(3)}"`，`_msg_counter` 每次调用 +1（**非线程安全**，多线程下可能重号）。
- `get_local_ips()`（第 278–286 行）：`socket.getaddrinfo(hostname, None, AF_INET)` 收集所有 IPv4，丢弃 `127.0.0.1`，返回 `sorted()` 结果；为空则返回 `["127.0.0.1"]`。
- **广播目标端口恒为 `cfg["udp_port"]`**（不是实际监听端口）。
- 本机 `_broadcast` 的报文自己也会收到，但被 `_udp_loop` 的 `uid == self.uid` 过滤掉。

### 2.5 TTL / 窗口常量速查

| 用途 | 值 |
| --- | --- |
| UDP 去重窗口 `SEEN_TTL` | 120 s |
| presence 在线判定 `PRESENCE_TTL` | 30 s |
| 在线表删除阈值 | `2 × PRESENCE_TTL` = 60 s |
| discovered 删除阈值 | `4 × PRESENCE_TTL` = 120 s |
| 附近内网 `NET_NEARBY_TTL` | 40 s |
| 入网投票有效期 `NET_VOTE_WINDOW` | 86400 s |
| 入网改票窗口 `NET_VOTE_CHANGE` | 300 s |
| 改名投票等待 `RENAME_VOTE_WAIT` | 120 s（过期提案容忍 +10 s） |

---

## 3. UDP 报文完整清单

### 3.1 全部 `"t"` 字符串字面量（第 759–804 行的 dispatch 表）

| # | `t` 值 | 处理函数 | 是否本文范围 |
| --- | --- | --- | --- |
| 1 | `presence` | `_on_presence(msg, addr)` | ✅ |
| 2 | `chat` | `_on_chat(msg)` | ✅ |
| 3 | `file_pub` | `_on_file_pub(msg)` | 文件部分（第 3.12 节登记字段） |
| 4 | `file_req` | `_on_file_req(msg, addr)` | 文件部分（第 3.13 节登记字段） |
| 5 | `file_have` | `_on_file_have(msg)` | 文件部分（第 3.14 节登记字段） |
| 6 | `file_sync_req` | `_on_sync_req(msg)` | 文件部分（第 3.15 节登记字段） |
| 7 | `log` | `_on_log(msg)` | ✅ |
| 8 | `group_announce` | `_on_group_announce(msg)` | ✅ |
| 9 | `rename_req` | `_on_rename_req(msg)` | ✅ |
| 10 | `rename_vote` | `_on_rename_vote(msg)` | ✅ |
| 11 | `dm` | `_on_dm(msg)` | ✅ |
| 12 | `net_scan` | `_on_net_scan(msg, addr)` | ✅ |
| 13 | `net_announce` | `_on_net_announce(msg, addr)` | ✅ |
| 14 | `net_join_req` | `_on_net_join_req(msg, addr)` | ✅ |
| 15 | `net_join_vote` | `_on_net_join_vote(msg)` | ✅ |
| 16 | `net_registry` | `_on_net_registry(msg, addr)` | ✅ |
| 17 | `net_member_left` | `_on_net_member_left(msg)` | ✅ |
| 18 | `net_join_result` | `_on_net_join_result(msg)` | ✅ |

**合计 18 个 type 字面量**（其中 15 个在本文范围内，4 个文件类）。
未识别的 `t` 值：**静默忽略**（dispatch 无 else 分支）。

### 3.2 公共信封（所有报文共有）

| JSON key | 类型 | 构造处 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 各发送方 | 报文类型 |
| `uid` | string | `setdefault` | 发送者设备唯一 ID（12 位大写十六进制 MAC，或 `RND`+12 位十六进制） |
| `name` | string | `setdefault` | 发送者昵称（`self.my_name`） |
| `msg_id` | string | `setdefault` | 去重 ID，格式 `<uid>-<counter>-<6 位 hex>` |

下表各报文中，**「信封」一行指上述 3 个字段**，不再重复。

### 3.3 `presence`（构造：`_presence_loop` 第 867–873 行；解析：`_on_presence` 第 812–834 行）

发送：每 `PRESENCE_INTERVAL`(8s) 广播一次，广播（非单播）。

| JSON key | 类型 | 发送端值 | 接收端读取 | 含义 |
| --- | --- | --- | --- | --- |
| `t` | string | `"presence"` | — | — |
| `ip` | string | `get_local_ips()[0]` | `msg.get("ip") or addr[0]` | 本机局域网 IP（**接收端在缺失时回退用 UDP 源地址**） |
| `port` | int | `self.my_tcp_port()` | `int(msg.get("port") or 0)` | TCP 服务端口 |
| `groups` | array\<object\> | `[{"gid":…,"gname":…}, …]` | `msg.get("groups", []) or []` | 我加入的群组列表，元素为 `{"gid":string,"gname":string}` |
| `ver` | string | `APP_VERSION` | `msg.get("ver", "")` | 程序版本号 |
| — | — | **无 `ts` 字段** | 接收端用本地 `now_ts()` 记时间 | 注意：presence 不自带时间戳 |
| 信封 | | | | `uid`,`name`,`msg_id` |

接收侧行为（`_on_presence`）：

```
uid = msg.get("uid"); 若空 → return
self.peers[uid] = {"name": str(msg.get("name","")),
                   "ip": msg.get("ip") or addr[0],
                   "tcp_port": int(msg.get("port") or 0),
                   "groups": msg.get("groups", []) or [],
                   "ver": msg.get("ver",""),
                   "ts": now_ts()}                       # 覆盖式更新
对 peers[uid]["groups"] 中每个含 gid 的元素 g:
    self.discovered.setdefault(gid, {"gname": g.get("gname", f"群{gid[:6]}"),
                                     "host_uid": uid,
                                     "host_name": self.peers[uid]["name"],
                                     "ts": now_ts()})     # 已存在则不覆盖
self._on_presence_hook_deliver(uid)                      # 触发保密转存送达（见 7.3）
self._emit("peers_changed")
```

### 3.4 `chat`（构造：`send_chat` 第 903–906 行；解析：`_on_chat` 第 910–919 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"chat"` | — |
| `gid` | string | 群 ID | 目标群 |
| `gname` | string | 群名 | 目标群名 |
| `text` | string | `str(text).strip()[:4000]` | 正文，最长 4000 字符 |
| `ts` | float | `now_ts()` | 发送时间（epoch 秒） |
| 信封 | | | `uid`,`name`,`msg_id` |

接收侧：`gid` 必须在本机 `self.groups` 中，否则丢弃；必须 `_member_ok(msg["uid"])`（内网准入检查，见 7.6），否则丢弃；然后 `_emit("chat", {...})`，其中 `text` 再截断到 4000 字符，`ts` 缺失时取 `now_ts()`。

### 3.5 `dm`（私聊；构造：`send_dm` 第 928 行；解析：`_on_dm` 第 933–942 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"dm"` | — |
| `to_uid` | string | 接收者 uid | 目标设备 ID |
| `text` | string | `str(text).strip()[:4000]` | 正文 |
| `ts` | float | `now_ts()` | 时间戳 |
| 信封 | | | `uid`,`name`,`msg_id` |

接收侧：`to_uid != self.uid` → 静默丢弃（不记日志）；`_member_ok(msg["uid"])` 不通过 → 丢弃；否则 `_emit("dm", {...})`。

> `send_dm` **不做内网准入检查**，只做参数合法性检查（`text` 非空、`to_uid` 非空且 ≠ 自己），失败返回 `False`。

### 3.6 `log`（公共日志；构造：`broadcast_log` 第 1289 行；解析：`_on_log` 第 1292–1296 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"log"` | — |
| `text` | string | `str(text)[:300]` | 日志正文，**发送端截断到 300 字符** |
| `ts` | float | `now_ts()` | 时间戳 |
| 信封 | | | `uid`,`name`,`msg_id` |

接收侧：`_member_ok(msg["uid"])` 不通过 → 丢弃（内网外机器不能写入公共日志）；否则 `_emit("log", "[MM-DD HH:MM:SS] <name>: <text>")`，时间取 `msg["ts"]`（缺失用 `now_ts()`）。

### 3.7 `group_announce`（群公告；构造：`create_group` 第 1107 行；解析：`_on_group_announce` 第 836–846 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"group_announce"` | — |
| `gid` | string | 群 ID | — |
| `gname` | string | 群名 | — |
| 信封 | | | `uid`,`name`,`msg_id`；接收端把 `msg["uid"]` 记为 `host_uid`、`msg["name"]` 记为 `host_name` |

**只在 `create_group` 时广播一次**；`join_group` / `leave_group` 不广播本报文。
接收侧：`gid` 为空 → 丢弃；否则 **覆盖式**写入 `self.discovered[gid] = {"gname": msg.get("gname", f"群{gid[:6]}"), "host_uid": msg.get("uid"), "host_name": msg.get("name",""), "ts": now_ts()}`；`_emit("peers_changed")`。

### 3.8 `rename_req`（改名提案；构造：`start_rename_vote` 第 1198–1199 行；解析：`_on_rename_req` 第 1245–1264 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"rename_req"` | — |
| `pid` | string | `self._new_msg_id()` | 提案 ID |
| `old` | string | 当前昵称 | 原名字 |
| `new` | string | `sanitize_name(new_name, 40)` | 新名字（≤40 字符，非法字符替换为 `_`） |
| `ts` | float | `now_ts()` | 提案时间 |
| 信封 | | | `uid`（提案人）、`name`（提案人当前昵称）、`msg_id` |

接收侧按顺序：

1. `pid` 为空或已在 `self._rename_requests` → 丢弃（每个提案只弹一次）。
2. `not _member_ok(msg["uid"])` → 丢弃。
3. `len(_rename_requests) > 1000` → **整体清空**该集合。
4. 加入 `_rename_requests`。
5. 若 `now_ts() - float(msg.get("ts") or 0) > RENAME_VOTE_WAIT + 10`（即 130 秒）→ 视为过期，**不再打扰用户**（但已记入去重集合）。
6. `_emit("rename_request", {"pid","from_uid","from_name","old","new"})`。

### 3.9 `rename_vote`（改名投票；构造：`cast_rename_vote` 第 1242–1243 行；解析：`_on_rename_vote` 第 1266–1284 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"rename_vote"` | — |
| `pid` | string | 提案 ID | — |
| `approve` | int | `1` 同意 / `0` 拒绝 | **用整数而非布尔** |
| `ts` | float | `now_ts()` | 投票时间 |
| 信封 | | | `uid` = 投票人 uid（提案人按此去重） |

接收侧：按 `pid` 取 `self._rename_votes[pid]`；不存在或 `done` → 丢弃；`uid = str(msg["uid"])` 为空、或已在 `approvers`/`rejectors` 中 → 丢弃（**同一 uid 只能投一次，不可改票**）；`msg["approve"]` 真值 → 加入 `approvers`，否则加入 `rejectors`；`_emit("rename_vote_progress", pid, approves, rejects)`；若 `approves > self.vote_need` → `_finish_rename_vote(pid)`。

### 3.10 `net_scan`（扫描附近内网；构造：`net_scan` 第 1513 行；应答：`_on_net_scan` 第 1648–1660 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"net_scan"` | — |
| `ts` | float | `now_ts()` | 时间戳 |
| 信封 | | | `uid`,`name`,`msg_id` |

接收侧（应答方）：

```
if not self.in_net(): return                    # 未入网者不应答
now = now_ts()
if now - self._net_reply_ts < 0.5: return        # 0.5 秒节流
self._net_reply_ts = now
self._net_announce()                             # 完整广播 net_announce（必须广播，不能单播）
```

> 源码注释（第 1651–1652 行）明确：应答必须用广播，因为同机多实例共用同一 UDP 端口时单播只会被其中一个 socket 收到。

### 3.11 `net_announce`（内网公告；构造：`_net_announce` 第 1589–1596 行；解析：`_on_net_announce` 第 1662–1675 行）

| JSON key | 类型 | 发送端值 | 接收端读取 | 含义 |
| --- | --- | --- | --- | --- |
| `t` | string | `"net_announce"` | — | — |
| `nid` | string | `self.net["nid"]` | 必填，空则丢弃 | 内网 ID（6 位大写十六进制） |
| `net_name` | string | `self.net.get("name","")` | `str(msg.get("net_name") or f"内网{nid[:6]}")` | 内网名 |
| `founder_mac` | string | `self.net.get("founder_mac","")` | `str(msg.get("founder_mac") or "")` | 发起者 uid |
| `member_count` | int | `len(self.net_members)` | `int(msg.get("member_count") or 0)` | 成员数 |
| `macs` | array\<string\> | `sorted(self.net_members)[:64]` | `list(msg.get("macs") or [])` | 成员 uid 列表（最多 64，**按字符串升序**） |
| `ts` | float | `now_ts()` | 本地覆盖为 `now_ts()` | 时间戳 |
| 信封 | | | | `uid`,`name`,`msg_id` |

发送时机：`net_create`、`net_rename`、`_net_join_success`、`_net_bootstrap`（已在内网）、`_net_tick`（每 ≥15 秒）、`_on_net_scan` 应答。
**若 `not self.in_net()` 则直接返回，不发送。**

接收侧：`nid` 空 → 丢弃；已在内网且 `nid == self.net["nid"]` → 丢弃（自己内网的公告）；否则写入
`self.net_nearby[nid] = {"nid","name","founder_mac","member_count","macs","ip": addr[0] if addr else "", "ts": now_ts()}`，并 `_emit("net_nearby_changed")`。

### 3.12 `net_join_req`（入网申请；构造：`_net_join_req_broadcast` 第 1614–1622 行；解析：`_on_net_join_req` 第 1677–1714 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"net_join_req"` | — |
| `req_id` | string | `self.net_applying["req_id"]`（由 `_new_msg_id()` 生成） | 申请 ID |
| `nid` | string | 目标内网 ID | — |
| `mac` | string | `self.uid` | 申请者设备 ID |
| `name` | string | `self.my_name` | 申请者昵称 |
| `ip` | string | `get_local_ips()[0]` | 申请者 IP |
| `tcp_port` | int | `self.my_tcp_port()` | 申请者 TCP 端口 |
| `voters` | array\<string\> | 申请者从 `net_nearby` 抄来的投票人 mac 列表 | 预期投票人 |
| `created_ts` | float | `net_applying["created_ts"]` | 申请创建时间 |
| `ts` | float | `now_ts()` | 本包发送时间 |
| 信封 | | | `uid`,`name`（=`name`，会被 `setdefault` 保留发送方已设值）,`msg_id` |

发送时机：`net_apply_join` 立即一次；之后 `_net_tick` 每 ≥ `NET_REQ_REBROADCAST`(10s) 重播一次；`_net_bootstrap` 若存在 `net_applying` 也重播一次。

接收侧顺序：

```
if not self.in_net(): return
if msg["nid"] != self.net["nid"]: return
req_id, mac = msg.get("req_id"), str(msg.get("mac") or "");  两者为空 → return
if mac in self.net_members:                                   # 已是成员
    self._net_sync(); return                                  # 广播登记表让他直接完成加入
req = self.net_reqs.get(req_id)
if req and req["state"] != "pending": return                   # 已结束的申请不再受理
if not req:
    created = float(msg.get("created_ts") or now_ts())
    req = {req_id, nid, mac, name|"?", ip | addr[0], tcp_port,
           voters: sorted(set(msg.voters) | {self.uid}),       # 我方也进入投票人集合
           votes: {}, state:"pending", note:"",
           created_ts: created, deadline: created + NET_VOTE_WINDOW}
    self.net_reqs[req_id] = req
    _emit("log", "...申请加入内网...: <net_vote_need 描述>")
    _emit("net_vote_changed")
else:
    vs = set(req.voters) | set(msg.voters) | {self.uid}
    if vs != set(req.voters): req["voters"] = sorted(vs)
self._save_net(); self._net_eval_req(req); self._net_sync()
```

### 3.13 `net_join_vote`（入网投票；构造：`net_cast_vote` 第 1582–1583 行、`_net_tick` 重播 第 2002–2004 行；解析：`_on_net_join_vote` 第 1716–1733 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"net_join_vote"` | — |
| `req_id` | string | 申请 ID | — |
| `nid` | string | 内网 ID | — |
| `voter` | string | `self.uid` | 投票人设备 ID |
| `voter_name` | string | `self.my_name` | 投票人昵称 |
| `approve` | bool \| null | `True` 同意 / `False` 拒绝 / `None` 弃权（JSON `null`） | 投票意向 |
| `ts` | float | 投票时间 | 重播时使用原投票时间 `mv["ts"]` |
| 信封 | | | `uid` = 投票人 uid；**解析方回退读取 `voter` 缺失时用 `uid`** |

接收侧：

```
req = self.net_reqs.get(msg["req_id"])
if not req or req["state"] != "pending": return
voter = str(msg.get("voter") or msg.get("uid") or "")
if not voter or voter not in req["voters"]: return
approve = msg.get("approve");  approve = None if approve is None else bool(approve)
ts = float(msg.get("ts") or now_ts())
old = req["votes"].get(voter)
if old and float(old["ts"]) >= ts: return          # 旧票（ts 不更新）忽略
req["votes"][voter] = {"approve": approve, "ts": ts, "name": str(msg.get("voter_name") or "")}
self._save_net(); _emit("net_vote_changed"); _net_eval_req(req)
```

> **入网投票允许改票**：以更新的 `ts` 覆盖旧票（0.3 版改名投票则不允许改票，两者行为不同）。

### 3.14 `net_registry`（内网登记表同步；构造：`_net_sync` 第 1598–1612 行；解析：`_on_net_registry` 第 1735–1795 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"net_registry"` | — |
| `nid` | string | 内网 ID | — |
| `net_name` | string | 内网名 | — |
| `founder_mac` | string | 发起者 uid | — |
| `members` | array\<member\> | `list(self.net_members.values())` | 成员登记表（结构见 3.14.1） |
| `reqs` | array\<req\> | 筛选后的申请列表（见下） | 申请与投票快照（结构见 3.14.2） |
| `ts` | float | `now_ts()` | — |
| 信封 | | | `uid`,`name`,`msg_id` |

`reqs` 筛选规则（第 1603–1607 行）：`nid` 匹配 **且**（`state == "pending"` **或** `now - closed_ts <= 3600`），然后取**最后 30 条**（`reqs[-30:]`，按 `self.net_reqs` 字典插入顺序）。

发送时机：`net_create`、`net_rename`、`_net_join_success`、`net_cast_vote`、`_on_net_join_req`、`_net_approve_req`、`_net_bootstrap`、`_net_tick`（每 ≥15 秒）。`not in_net()` 时不发送。

#### 3.14.1 `member` 对象（`self.net_members` 的值）

| key | 类型 | 含义 |
| --- | --- | --- |
| `mac` | string | 设备唯一 ID（= 全局的 uid） |
| `name` | string | 昵称，默认 `"?"` |
| `ip` | string | 局域网 IP，默认 `""` |
| `tcp_port` | int | TCP 端口，默认 `0` |
| `joined_ts` | float | 加入时间（本地时间，首次登记时写入） |
| `founder` | bool | 是否内网发起者 |

#### 3.14.2 `req` 对象（`self.net_reqs` 的值 / `net_reqs` 元素）

| key | 类型 | 含义 |
| --- | --- | --- |
| `req_id` | string | 申请 ID |
| `nid` | string | 内网 ID |
| `mac` | string | 申请者 uid |
| `name` | string | 申请者昵称，默认 `"?"` |
| `ip` | string | 申请者 IP |
| `tcp_port` | int | 申请者 TCP 端口 |
| `voters` | array\<string\> | 有投票权的成员 uid 列表（有序，`sorted`） |
| `votes` | object\<uid → vote\> | 投票表，见下 |
| `state` | string | `"pending"` / `"approved"` / `"rejected"` |
| `note` | string | 结论文案 |
| `created_ts` | float | 创建时间 |
| `deadline` | float | 截止时间 = `created_ts + NET_VOTE_WINDOW` |
| `closed_ts` | float | **仅结束时写入**（`_net_approve_req` / `_net_reject_req`）；未结束时该键不存在 |

`vote` 对象：`{"approve": bool|null, "ts": float, "name": string}`。

接收侧 `_on_net_registry` 分两种情形：

**A. 我已在该内网（`nid == self.net["nid"]`）**

```
changed = False
for m in msg.members:  changed |= self._net_add_member(m.mac, m.name, m.ip, m.tcp_port, founder=m.founder)
for r in msg.reqs:     changed |= self._merge_req(r)
for r in self.net_reqs.values():  若 state==pending 且 nid==nid: _net_eval_req(r)
if changed: self._save_net()
_emit("net_changed"); _emit("net_vote_changed"); return
```

**B. 我还没入网，且这正是我在申请的内网（`nid == net_applying["nid"]`）**

```
若我的 uid 出现在 msg.members 的 mac 集合中 → self._net_join_success(msg); return
（否则）对 msg.reqs 中 req_id == 我的申请 ID 的条目：
    逐条合并 votes（远端 ts 更大才覆盖）、合并 voters
    若远端 state == "rejected" → 本地 state 置 rejected、note 抄录 → rejected 变量记下原因
若 rejected: net_applying = None; _save_net()
             _emit("net_result", False, "入网申请未通过：<原因>"); _emit("net_vote_changed"); _emit("net_changed"); return
若 changed:  _emit("net_vote_changed"); _emit("net_changed")
```

`_merge_req(remote)`（第 1797–1829 行）：本地无该 `req_id` → 全量新建（`deadline` 缺省 `created_ts + NET_VOTE_WINDOW`），返回 `True`；否则合并 `voters` 并集、按 `ts` 合并 `votes`、`pending → approved/rejected` 状态迁移并抄 `note`，任一处变化返回 `True`。

### 3.15 `net_member_left`（退出内网；构造：`net_leave` 第 1469 行；解析：`_on_net_member_left` 第 1831–1839 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"net_member_left"` | — |
| `nid` | string | 内网 ID | — |
| `mac` | string | `self.uid` | 退出者 uid |
| `name` | string | `self.my_name` | 退出者昵称 |
| **无 `ts`** | | | 源码未定义时间戳字段 |
| 信封 | | | `uid`,`name`,`msg_id` |

接收侧：`not in_net()` 或 `nid` 不匹配 → 丢弃；`mac` 非空且确实从 `net_members` 中弹出 → `_save_net()`，`_emit("log", "...退出了内网")`，`_emit("net_changed")`。

### 3.16 `net_join_result`（入网结果否决通知；构造：`_net_reject_req` 第 1906–1908 行；解析：`_on_net_join_result` 第 1841–1856 行）

| JSON key | 类型 | 发送端值 | 含义 |
| --- | --- | --- | --- |
| `t` | string | `"net_join_result"` | — |
| `req_id` | string | 申请 ID | — |
| `nid` | string | 内网 ID | — |
| `mac` | string | 申请者 uid | — |
| `approved` | bool | **恒为 `False`**（源码只广播拒绝） | — |
| `reason` | string | 结论文案 | — |
| `ts` | float | `now_ts()` | — |
| 信封 | | | `uid`,`name`,`msg_id` |

发送条件（第 1904–1905 行）：`macs = sorted(set(req.voters) | {req.mac})`，**只有当 `macs[0] == self.uid` 时才广播**——即由「投票人 ∪ 申请人」中 uid 字典序最小者统一宣布，避免重复广播。

接收侧：`net_applying` 不存在或 `req_id` 不匹配 → 丢弃；`approved` 为真 → **直接 return（忽略）**；否则清空 `net_applying`、把本地 req 置 `rejected` 并抄 `note`、`_save_net()`、`_emit("net_result", False, "入网申请未通过：<reason>")`、`_emit("net_vote_changed")`、`_emit("net_changed")`。

### 3.17 文件类报文（**字段登记，算法细节见后续文档**）

| `t` | 构造处 | 关键字段 |
| --- | --- | --- |
| `file_pub` | `send_file_to_group` 第 2131–2135 行；`_on_sync_req` 重播 第 2206–2211 行 | `gid`,`gname`,`file:{id,name,size}`,`ts`,`sender_uid`,`sender_name`；重播时额外带 `"resync": true` |
| `file_sync_req` | `_sync_req` 第 2196 行 | `gid`,`gname`（**无 `ts`**） |
| `file_req` | `_public_request` 第 2287–2293 行 | `req_id`,`gid`,`gname`,`sender_uid`,`sender_name`,`file:{id,name,size}`,`ts` |
| `file_have` | `_on_file_req` 第 2308–2312 行 | `req_id`,`file_id`,`ip`(`get_local_ips()[0]`),`port`(`my_tcp_port()`),`ts` |

---

## 4. TCP 传输层

### 4.1 帧格式（第 459–482 行）

```
帧 = 4 字节大端无符号整数长度 N  +  N 字节 UTF-8 JSON 文本
```

| 函数 | 行为 |
| --- | --- |
| `encode_frame(payload)` | `struct.pack(">I", len(payload)) + payload` |
| `recv_exact(sock, n)` | 循环 `recv` 直到收满 n 字节；对端关闭时抛 `ConnectionError("连接被对端关闭")` |
| `send_json(sock, obj)` | `sock.sendall(encode_frame(json.dumps(obj, ensure_ascii=False).encode("utf-8")))` |
| `recv_json(sock)` | 读 4 字节头 → `struct.unpack(">I")`；若 `n > 64*1024*1024` 抛 `ValueError("异常的数据帧长度")`；再读 n 字节并 `json.loads(utf-8)` |

**移植要点**：长度字段是**大端**；Python 端 `recv` 只保证「不超过」，C++ 必须同样循环读取；上限 64 MiB。

### 4.2 TCP 服务端建立（`_make_tcp`，第 690–706 行）

```
AF_INET, SOCK_STREAM, SO_REUSEADDR
port = int(cfg["tcp_port"])
依次尝试 p ∈ {port, port+1, …, port+20}（共 21 个端口）：
    bind(("0.0.0.0", p)); listen(16)
    若 p != port: cfg["tcp_port"] = p; save_config(cfg)
    成功即 break
全部失败 → raise RuntimeError("无法绑定 TCP 端口")
```

- `my_tcp_port()`（第 708 行）：优先返回 `self._tcp_srv.getsockname()[1]`；服务未建立时返回 `cfg["tcp_port"]`。
- **TCP 端口只有一个用途族**：接收点对点请求（文件下载 + 保密转存的存取），没有第二个 TCP 监听端口。

### 4.3 接受循环（`_tcp_loop`，第 2396–2404 行）

```
while self._running:
    try: conn, addr = self._tcp_srv.accept()
    except OSError: break
    每个连接起一个线程（name="tcp-conn", daemon=True）执行 _handle_conn(conn, addr)
```

### 4.4 单连接握手与命令分发（`_handle_conn`，第 2406–2440 行）

```
conn.settimeout(30)                    # 服务端每连接总超时 30 秒
req = recv_json(conn)                  # 客户端必须先发一帧请求，服务端只读一次
```

| 请求 `cmd` | 请求字段 | 成功应答 | 失败应答 |
| --- | --- | --- | --- |
| `get` | `file_id` (string) | 第 1 帧：`{"cmd":"data","file_id":<fid>,"name":<index[fid].name 或 "">,"size":<cache 文件字节数>}`；紧接着**裸字节流**（无帧头），按 `FILE_CHUNK`(128 KiB) 分块发送至 EOF | `{"cmd":"err","err":"没有该文件"}`（`fid` 为空或缓存文件不存在） |
| `secret_store` | `store_id`,`to_uid`,`from_uid`,`from_name`,`blob`,`key`,`sig`,`ts`,`retries` | **恒回** `{"cmd":"secret_stored","store_id":<store_id 或 "">}`（无论内部校验是否通过） | 无（校验失败也回成功帧） |
| `secret` | `store_id`,`to_uid`,`from_uid`,`from_name`,`blob`,`key`,`sig`,`ts`（**不含 `retries`**） | `{"cmd":"secret_ack","store_id":<store_id 或 "">,"ok":true}` | `{"cmd":"secret_ack","store_id":<store_id 或 "">,"ok":false}` |
| 其它 | — | — | `{"cmd":"err","err":"未知命令"}` |

- 所有异常被 `except Exception: pass` 吞掉（客户端会因读不到应答而超时失败）。
- `finally` 中一定 `conn.close()`。
- 握手顺序：**客户端先发请求帧 → 服务端回 1 帧应答 →（仅 `get` 成功时）服务端续发裸数据流 → 关闭**。没有版本协商、没有认证、没有 hello。

### 4.5 各 TCP 超时一览

| 位置 | 超时 |
| --- | --- |
| `_make_udp` | `settimeout(1.0)` |
| `_handle_conn`（服务端） | `conn.settimeout(30)` |
| `_store_secret_on`（客户端存转存） | `settimeout(6)` |
| `_deliver_one`（客户端送转存） | `settimeout(8)` |
| `_fetch_from`（客户端下文件） | 连接与头帧 `settimeout(8)`，收到头帧后改为 `settimeout(60)` |

---

## 5. PeerCore 内部状态字典（`__init__`，第 568–623 行）

| 成员名 | 类型 | 键 | 值结构 | 说明 |
| --- | --- | --- | --- | --- |
| `self.cfg` | dict | — | `load_config()` 的结果 | 配置 |
| `self.uid` | string | — | `cfg["uid"]` | 本机唯一 ID |
| `self.events` | `queue.Queue` | — | — | 事件队列 |
| `self._name_getter` | callable | — | — | 昵称取值器 |
| `self.data_root` | Path | — | — | 数据根 |
| `self.groups` | dict | gid (string, 6 位大写 hex) | `{"gid": string, "gname": string}` | 我加入的群 |
| `self.peers` | dict | uid (string) | `{"name":string,"ip":string,"tcp_port":int,"groups":[{gid,gname}],"ver":string,"ts":float}` | 在线表，**覆盖式更新** |
| `self.discovered` | dict | gid (string) | `{"gname":string,"host_uid":string,"host_name":string,"ts":float}` | 发现的群 |
| `self._seen` | dict | msg_id (string) | float（收到时间） | UDP 去重表 |
| `self._msg_counter` | int | — | — | `msg_id` 自增计数，初始 `0` |
| `self.index` | dict | file_id (sha256 hex) | `{"name":string,"size":int,"ts":float,"src_uid":string,"src_name":string,"groups":list<string>,"has_local":bool}` | 文件索引（持久化 `data/index.json`） |
| `self._secrets` | dict | store_id (string) | `{"to_uid","from_uid","from_name","blob","key","sig","ts","retries"}` | 我替别人暂存的转存（持久化 `data/secret_relay.json`） |
| `self._recv_secrets` | set\<string\> | store_id | — | 我已收到的转存编号（持久化 `data/secret_received.json`） |
| `self._delivering` | set\<string\> | uid | — | 正在送达的目标，防并发重复 |
| `self._offers` | dict | req_id (string) | `list[(ip:string, port:int, uid:string)]` | 公共询问的应答队列（文件部分） |
| `self._downloading` | set\<string\> | file_id | — | 正在下载（文件部分） |
| `self._download_queue` | `queue.Queue` | — | task dict | 下载任务队列（文件部分） |
| `self._active_downloads` | dict | file_id | task dict | 供 GUI 查询进度（文件部分） |
| `self._running` | bool | — | 初始 `True` | 运行标志 |
| `self._lock` | `threading.RLock()` | — | — | 可重入锁 |
| `self._udp_sock` | socket \| None | — | 初始 `None` | UDP socket |
| `self._tcp_srv` | socket \| None | — | 初始 `None` | TCP 监听 socket |
| `self._threads` | list\<Thread\> | — | — | 所有线程（含临时线程），`stop()` 时 join |
| `self.file_offers` | list | — | 初始 `[]` | **源码中定义后从未被读写**（第 604 行），移植可省 |
| `self._rename_votes` | dict | pid (string) | `{"old":string,"new":string,"approvers":set<uid>,"rejectors":set<uid>,"done":bool,"deadline":float}` | 我发起的改名投票 |
| `self._rename_requests` | set\<string\> | pid | — | 已展示过弹窗的提案（去重） |
| `self._my_votes` | set\<string\> | pid | — | 我已投过票的改名提案（同一提案只计一票） |
| `self.net` | dict \| None | — | `{"nid":string,"name":string,"founder_mac":string,"created_ts":float}`；加入他人内网时额外有 `"joined_ts":float` | 我所在的内网；`None` 表示未入网 |
| `self.net_members` | dict | mac (string) | `{"mac","name","ip","tcp_port","joined_ts","founder"}` | 内网成员登记表 |
| `self.net_reqs` | dict | req_id (string) | 见 3.14.2 | 入网申请（含投票与状态） |
| `self.net_nearby` | dict | nid (string) | `{"nid","name","founder_mac","member_count","macs","ip","ts"}` | 附近发现的内网 |
| `self.net_my_votes` | dict | req_id (string) | `{"approve": bool\|None, "ts": float}` | 我在入网投票中的选择（**不含 `name`**） |
| `self.net_applying` | dict \| None | — | `{"req_id","nid","name","voters","created_ts","deadline"}` | 我正在等待审批的申请 |
| `self._net_last_announce` | float | — | 初始 `0.0` | 上次公告时间 |
| `self._net_last_sync` | float | — | 初始 `0.0` | **源码中从未被读写**（第 619 行） |
| `self._net_last_req` | float | — | 初始 `0.0` | 上次申请重播时间 |
| `self._net_last_vote_replay` | float | — | 初始 `0.0` | 上次我的票重播时间 |
| `self._net_reply_ts` | float | — | 初始 `0.0` | 扫描应答节流时间戳 |
| `self._net_suppress_auto` | bool | — | **未在 `__init__` 中初始化**，仅在 `net_create`(False) / `net_leave`(True) / `net_apply_join`(False) 中赋值；`_net_bootstrap` 用 `getattr(self, "_net_suppress_auto", False)` 读取（默认 False） | 手动退出后是否禁止自动入网 |

### 5.1 持久化文件

| 文件（相对 `data_root`） | 内容 | 写入方 |
| --- | --- | --- |
| `config.json` | `cfg`（含 `name`/`uid`/`tcp_port`/`udp_port`/`groups`/`rename_count`/`rename_vote_need`…） | `save_config`（全局函数） |
| `net.json` | `{"net":…, "members":[member…], "reqs":[req…], "my_votes":{…}, "applying":…}`，`json.dumps(..., ensure_ascii=False, indent=1)` | `_save_net`（第 1326 行） |
| `index.json` | `self.index`，`indent=1` | `_save_index` |
| `secret_relay.json` | `self._secrets`，无缩进 | `_save_secrets` |
| `secret_received.json` | `sorted(self._recv_secrets)`，无缩进 | `_save_recv_secrets` |

`_load_net` 载入 `net.json` 后，若 `net["nid"]` 存在，会**立即用本机信息调用 `_net_add_member(self.uid, self.my_name, get_local_ips()[0], cfg["tcp_port"])`** 刷新自己的登记项（第 1321–1322 行）。

---

## 6. 推给 GUI 的 `queue` 事件

### 6.1 重要修正：队列里不是 `{"kind": …}` 字典

**源码事实**：`_emit`（第 672–676 行）执行 `self.events.put((ev,) + payload)`，即队列元素是 **元组 `(事件名, *载荷)`**，不是带 `kind` 键的字典。
GUI 侧 `_poll_events`（第 3732 行）取 `kind = ev[0]`，再按位置索引 `ev[1]`、`ev[2]`… 解析。
（`{"kind": ...}` 只在**聊天消息 dict** 里出现：`_msg_kind_row` 处理的消息带 `"t"` 键，见第 6.3 节。）

若 C++ 侧要改成 `{"kind": …, …}` 形式，等价映射为：`{"kind": ev[0], "payload": [ev[1:]]}`。

### 6.2 PeerCore（第 567–2035 行）内发出的全部事件

| `kind`（`ev[0]`） | 载荷（位置参数） | 触发点 | 说明 |
| --- | --- | --- | --- |
| `syslog` | `ev[1]`: string | `start()` 第 650 行 | 启动日志 |
| `peers_changed` | 无 | `_on_presence`、`_on_group_announce`、`_sweep_loop` | 要求 GUI 刷新成员列表 |
| `chat` | `ev[1]`: dict | `send_chat`、`_on_chat` | 见 6.3 |
| `dm` | `ev[1]`: dict | `send_dm`、`_on_dm`、`_on_secret_deliver` | 见 6.3 |
| `dm_sent` | `ev[1]`: dict | `_relay_dm_worker`（两处） | 见 6.3 |
| `groups_changed` | 无 | `create_group`、`join_group`、`leave_group` | |
| `name_changed` | `ev[1]`: string（新昵称） | `_apply_rename` | |
| `rename_vote_progress` | `ev[1]`: pid(string), `ev[2]`: approves(int), `ev[3]`: rejects(int) | `start_rename_vote`(0,0)、`_on_rename_vote` | |
| `rename_vote_done` | `ev[1]`: pid, `ev[2]`: ok(bool), `ev[3]`: 文案(string), `ev[4]`: 新名字(string) | `_finish_rename_vote` | |
| `rename_request` | `ev[1]`: dict `{"pid","from_uid","from_name","old","new"}` | `_on_rename_req` | 提示本机用户投票 |
| `log` | `ev[1]`: string（已格式化 `[MM-DD HH:MM:SS] 名字: 内容`） | `broadcast_log`、`_on_log`、`_on_net_join_req`、`_on_net_member_left`、`_net_reject_req` | |
| `net_status` | `ev[1]`: string | `net_create`、`net_leave`、`net_scan`、`net_apply_join`、`net_cancel_apply`、`_net_bootstrap` | 状态栏提示 |
| `net_changed` | 无 | 内网各流程 | 刷新内网页 |
| `net_vote_changed` | 无 | 内网投票各流程 | 刷新投票面板/红点 |
| `net_nearby_changed` | 无 | `net_scan`、`_on_net_announce`、`_net_tick` | 附近内网列表变化 |
| `net_result` | `ev[1]`: ok(bool), `ev[2]`: 文案(string) | `_on_net_registry`、`_on_net_join_result`、`_net_tick`、`_net_join_success` | 入网成功/失败弹窗 |

### 6.3 事件里携带的 dict 结构

`chat` 事件 dict：

| key | 类型 | 含义 |
| --- | --- | --- |
| `gid` | string | 群 ID |
| `gname` | string | 群名 |
| `from_name` | string | 发送者昵称（收到时缺省 `"?"`） |
| `from_uid` | string | 发送者 uid |
| `text` | string | 正文（≤4000 字符） |
| `ts` | float | 时间戳 |
| `self` | bool | `True` = 本机发出 |

`dm` 事件 dict：

| key | 类型 | 含义 |
| --- | --- | --- |
| `to_uid` | string | 会话对方 uid |
| `from_uid` | string | 消息来源 uid |
| `from_name` | string | 来源昵称（缺省 `"?"`） |
| `text` | string | 正文 |
| `ts` | float | 时间戳 |
| `self` | bool | 是否本机发出 |
| `relayed` | bool | **仅 `_on_secret_deliver` 发出时存在**，恒 `True`（表示经由保密转存送达） |

`dm_sent` 事件 dict（两形态）：

| 成功（`_relay_dm_worker` 第 973 行） | 失败（第 977 行） |
| --- | --- |
| `{"to_uid":string, "ok":true, "direct":false, "relay_name":string, "text":string, "ts":float}` | `{"to_uid":string, "ok":false, "reason":"对方不在线，且没有其他在线同学可代为转存"}` |

> **语义提示**：`direct` 恒为 `false`——直发成功时 `send_dm_auto` 直接返回 `{"ok":True,"direct":True}` 而**不发** `dm_sent` 事件（第 952–954 行）。

### 6.4 本文范围外但同一队列的 `kind`（第 3732–3792 行注册，供 C++ 事件表完整）

| `kind` | 载荷 | 来源 |
| --- | --- | --- |
| `syschat` | `ev[1]`: gid, `ev[2]`: text | 文件部分（`send_file_to_group` 等） |
| `file_new` | `ev[1]`: dict `{"file":…, "gid":…, "from_self":bool}` | 文件部分 |
| `file_status` | `ev[1]`: dict `{"id","status","progress","note"[,"path"]}` | 文件部分 |
| `notify` | `ev[1]`: title, `ev[2]`: body | 文件部分（系统通知） |
| `update` | `ev[1]`: dict `{"ok","found","remote","notes"[,"reason"]}` | 更新检查 |

GUI 事件轮询周期：`self.root.after(100, self._poll_events)` —— **每 100 ms** 排空一次队列（第 3792 行）。

---

## 7. 线程与循环

### 7.1 `start()` / `stop()`（第 634–660 行）

`start()`：

```
self._running = True
self._udp_sock = _make_udp()
self._tcp_srv = _make_tcp()
启动 6 个守护线程（daemon），线程名 "core-<name>"，全部包在 _safe() 里：
    ("udp",      _udp_loop)
    ("tcp",      _tcp_loop)
    ("presence", _presence_loop)
    ("sweep",    _sweep_loop)
    ("download", _download_worker)      # 文件部分
    ("net",      _net_loop)
每个线程追加到 self._threads
_emit("syslog", f"程序启动完成，局域网地址 {', '.join(get_local_ips())}")
```

`stop()`：

```
self._running = False
关闭 self._udp_sock（异常忽略）
对 self._threads 每个线程 t.join(timeout=1.5)
```

> `stop()` **不关闭** TCP 监听 socket；`_tcp_loop` 靠 `accept()` 抛 OSError 退出——若不 close，接受循环可能阻塞在 accept 上（`_tcp_srv` 无超时）。**这是源码行为，移植时需特别注意**（见第 9 章风险）。
> `start()` 支持 `stop()` 后再调用（下线后重新上线）。重复 `start()` 会再起一批线程，源码不禁止。

`_safe(fn)`（第 662–670 行）：包装函数，捕获 `Exception`，**仅当 `self._running` 为真时**打印堆栈，返回 `None`。

### 7.2 循环清单

| 线程名 | 函数 | 循环结构 | 时间参数 |
| --- | --- | --- | --- |
| `core-udp` | `_udp_loop` | `while self._running: recvfrom(65536)` | socket 超时 1.0 s（用于周期性检查 `_running`） |
| `core-tcp` | `_tcp_loop` | `while self._running: accept()` | 无超时；每连接线程另设 30 s |
| `core-presence` | `_presence_loop` | `while self._running: 广播 presence; sleep` | `PRESENCE_INTERVAL` = 8 s（**先发后睡**，启动即发） |
| `core-sweep` | `_sweep_loop` | `while self._running: sleep(3); 清理` | 3 s |
| `core-download` | `_download_worker` | `while self._running: queue.get(timeout=1.0)` | 1.0 s（文件部分） |
| `core-net` | `_net_loop` | `sleep(0.4)` → `_net_bootstrap()` → `while self._running: _net_tick(); sleep(2.0)` | 0.4 s / 2.0 s |
| 临时：`dm-relay` | `_relay_dm_worker` | 一次性 | — |
| 临时：`secret-deliver` | `_deliver_secrets_to` | 一次性（每个上线 uid 一个） | — |
| 临时：`rename-vote` | `_rename_vote_waiter` | `while running and not done and now < deadline: sleep(0.3)` | 0.3 s，上限 `RENAME_VOTE_WAIT`=120 s |
| 临时：`tcp-conn` | `_handle_conn` | 每连接 | 30 s |
| 临时：`net-rescan` | `_net_rescan_after_leave` | `net_scan(NET_SCAN_WINDOW)` 一次性 | 3 s |

### 7.3 `_net_tick()` 周期任务（每 2 秒，第 1986–2023 行）

```
now = now_ts()
if self.in_net():
    if now - _net_last_announce >= 15:            # NET_ANNOUNCE_INTERVAL
        _net_last_announce = now
        _net_announce(); _net_sync()
    for req in net_reqs.values():                 # 每个 pending 申请都重新评估
        if req["state"] == "pending": _net_eval_req(req)
    if now - _net_last_vote_replay >= 60:         # 每 60 秒重播我的票
        _net_last_vote_replay = now
        for rid, mv in net_my_votes.items():
            req = net_reqs.get(rid)
            if req and req["state"] == "pending":
                广播 net_join_vote(req_id=rid, nid=req.nid, voter=uid,
                                   voter_name=my_name, approve=mv.approve, ts=mv.ts)
    _net_prune()
a = self.net_applying
if a:
    if now - _net_last_req >= 10:                 # NET_REQ_REBROADCAST
        _net_last_req = now; _net_join_req_broadcast(); _emit("net_vote_changed")
    if now > a["deadline"]:
        self.net_applying = None
        req = net_reqs.get(a.req_id)
        if req: req["state"]="rejected"; req["note"]="投票超过 1 天，自动结束"
        _save_net()
        _emit("net_result", False, "入网申请超过 1 天仍未被通过，投票已结束")
        _emit("net_changed")
for nid in [k for k,v in net_nearby.items() if now - v["ts"] > NET_NEARBY_TTL(40)]:
    net_nearby.pop(nid); _emit("net_nearby_changed")
```

> 注意：`_net_prune` 只在 `in_net()` 为真时执行。

### 7.4 启动引导 `_net_bootstrap()`（第 1953–1984 行）

```
if in_net():
    _net_announce(); _net_sync()
    _emit("net_changed"); _emit("net_status", "已在内网「X」（成员 N 人）"); return
if net_applying:
    _emit("net_status", "正在等待内网成员投票…")
    _net_last_req = 0.0; _net_join_req_broadcast(); _emit("net_changed"); return
found = net_scan(NET_SCAN_WINDOW)                 # 阻塞 3 秒
if getattr(self, "_net_suppress_auto", False):
    _emit("net_status", "已退出内网（附近发现 N 个内网）：可在「内网」页手动加入或自行建立")
    _emit("net_changed"); return
if found:
    best = found[0]                               # ⚠ 未排序：net_nearby 字典的插入顺序
    _emit("net_status", "发现内网「…」（N 人），正在申请加入…")
    try: net_apply_join(best["nid"])
    except: traceback.print_exc(); net_create()
else:
    net_create()
```

> ⚠ **注意**：`net_scan` 返回的是 `[n for n in self.net_nearby.values() if now - n["ts"] <= NET_NEARBY_TTL]`，**未经排序**（`net_nearby_list()` 的排序只用于 GUI 展示）。因此 `found[0]` 是「最先被发现的」而非「人数最多的」。移植时必须保持一致。

---

## 8. 关键算法

### 8.1 presence 在线判定

```
is_online(uid):
    p = self.peers.get(uid)
    return p 存在 AND (now_ts() - p["ts"] <= PRESENCE_TTL(30)) AND p["tcp_port"] 为真值
```

- 三个条件缺一不可；`tcp_port == 0` 视为不在线。
- `group_members(gid)`（第 852–861 行）用更弱的条件：只判断 `now - ts <= PRESENCE_TTL` 且该 peer 的 `groups` 里有该 gid，**不要求 tcp_port**。返回列表第一项恒为本人 `{"name":my_name,"uid":self.uid,"ip":"本机","self":True}`。
- `_sweep_loop` 用 `> 2 × PRESENCE_TTL`(60 s) 才删除 peers，`> 4 × PRESENCE_TTL`(120 s) 才删除 discovered。

### 8.2 日志合并去重

**源码未实现日志合并去重。** 公共日志是纯 UDP 单向广播（`log` 报文），接收方直接追加到 GUI 文本控件：

- 唯一去重是 UDP 层的 `msg_id` + `SEEN_TTL`(120 s) 窗口（第 762–767 行）。
- 没有历史补齐、没有序号、没有向量时钟。超过 120 秒后重复到达的同一 `msg_id` 会被再次显示。
- 本机自己发出的日志由 `broadcast_log` 直接本地 `_emit("log", …)` 显示，**不经过 UDP**，因此不会重复。
- `LOG_MAX_LINES = 800` 只是 GUI 控件行数上限（第 4194–4195 行），PeerCore 不参与。

### 8.3 内网准入投票计数与通过判定

**投票门槛函数 `net_vote_need(members)`（第 183–199 行）**：

```
members = max(0, int(members))
if members <= 5:  return 0,  "免投票自动通过"
if members <= 7:  return members, "需全票通过"
return members // 3 + 1, "需超过 1/3 同意"          # 整数除法后 +1 ⇒ 严格超过 1/3
```

**计数 `_req_progress(req)`（第 1372–1381 行）**：

```
voters   = list(req["voters"])
votes    = req["votes"]
approvals = [m for m in voters if votes.get(m, {}).get("approve") is True]
rejects   = [m for m in voters if votes.get(m, {}).get("approve") is False]
abstains  = [m for m in voters if votes.get(m, {}).get("approve") is None and m in votes]
need, desc = net_vote_need(len(voters))            # ⚠ 分母是 voters 长度，不是内网成员数
返回 {"voters":len, "need":need, "desc":desc, "approvals":len, "rejects":len,
      "abstains":len, "approvers":list, "rejecters":list}
```

**判定 `_net_eval_req(req)`（第 1859–1884 行）** —— 严格按此顺序短路：

```
1. req["state"] != "pending"                          → return
2. not in_net() or req["nid"] != self.net["nid"]      → return
3. req["mac"] in self.net_members                     → state="approved"; note="已是内网成员"
                                                        → return（⚠ 不 _save_net、不 _net_sync）
4. prog = _req_progress(req); need = prog.need
5. need == 0                                          → _net_approve_req(req, "<desc>，自动通过")
6. prog.approvals >= need                             → _net_approve_req(req, "同意 a/v 票，已达门槛（<desc>）")
7. prog.rejects > 0 and need >= voters                → _net_reject_req(req, "有 R 名成员反对，无法全票通过")
8. now > req["deadline"]                              → _net_reject_req(req, "投票已满 24 小时仍只有 A 票（需 N 票），投票结束")
```

`_net_approve_req`（第 1886–1897 行）：`state="approved"`、`note`、`closed_ts=now_ts()` → `_net_add_member(req.mac, req.name, req.ip, req.tcp_port)` → `_save_net()` → `_net_sync()` → `broadcast_log(...)` → `_emit("net_changed")`、`_emit("net_vote_changed")`。

`_net_reject_req`（第 1899–1912 行）：`state="rejected"`、`note`、`closed_ts=now_ts()` → `_save_net()` → 若 `sorted(set(voters) | {req.mac})[0] == self.uid` 则广播 `net_join_result` → `_emit("log", …)` → `_emit("net_changed")`、`_emit("net_vote_changed")`。

**成员登记 `_net_add_member(mac, name, ip, tcp_port, founder=False)`（第 1624–1645 行）**：

```
mac 为空 → 返回 False
新成员 → net_members[mac] = {mac, name: name or "?", ip: ip or "",
                             tcp_port: int(tcp_port or 0), joined_ts: now_ts(),
                             founder: bool(founder or (net 存在 and net.founder_mac == mac))}
         → 返回 True
老成员 → 对 ("name",name),("ip",ip),("tcp_port",tcp_port)：值非空且与旧值不同才更新（tcp_port 转 int）
         founder 为真且原不为真 → 置 True
         → 返回「是否有任何变化」
```

### 8.4 改名投票计数与通过判定

```
vote_need property      = int(cfg.get("rename_vote_need", RENAME_VOTE_NEED))，异常回退 5
rename_count property   = int(cfg.get("rename_count", 0) or 0)，异常回退 0
rename_needs_vote()     = rename_count > 0
```

判定在 `_finish_rename_vote(pid)`（第 1217–1232 行）：

```
加锁：v = _rename_votes.get(pid)；不存在或 done → return；置 done = True（提前置位防重入）
approves = len(v["approvers"])
if approves > self.vote_need:                    # 严格大于
    _apply_rename(v["new"], v["old"], note=f"（{approves} 名用户投票通过）")
    _emit("rename_vote_done", pid, True, "投票通过：{approves} 名用户同意（需超过 {vote_need} 名）", v["new"])
else:
    _emit("rename_vote_done", pid, False, "同意票不足：收到 {approves} 票，需超过 {vote_need} 票", v["new"])
加锁：_rename_votes.pop(pid)
```

- 触发时机：`_rename_vote_waiter` 超时/完成，或 `_on_rename_vote` 中 `approves > vote_need` 立即调用。
- **反对票不影响通过**（只有 `approvers` 计数参与判断）；**弃权在改名投票中不存在**。
- 投票人集合按 `approvers`/`rejectors` 成员去重，同一 uid 第二次投票被丢弃（不可改票）。
- `cast_rename_vote` 本机侧用 `_my_votes` 保证「同一提案只投一票」；`len(_my_votes) > 1000` 时整体清空。
- 提案过期：接收方在 `now - msg.ts > RENAME_VOTE_WAIT + 10`(130 s) 时不弹窗。

`_apply_rename(new_name, old, note)`（第 1173–1178 行）：

```
cfg["name"] = new_name
cfg["rename_count"] = rename_count + 1
save_config(cfg)
broadcast_log(f"「{old}」更名为「{new_name}」{note}".rstrip())
_emit("name_changed", new_name)
```

### 8.5 保密转存：选择与送达重试

**发送侧发起（`send_dm_auto` → `_relay_dm_worker`，第 945–978 行）**：

```
send_dm_auto(to_uid, text):
    text = str(text).strip()[:4000]
    参数非法（text 空 / to_uid 空 / to_uid == self.uid）→ 返回 {"ok":False,"reason":"参数无效"}
    if is_online(to_uid): send_dm(to_uid, text); return {"ok":True,"direct":True}
    起线程 dm-relay 执行 _relay_dm_worker(to_uid, text)
    return {"ok":True,"pending":True}

_relay_dm_worker(to_uid, text):
    cands = [p for uid,p in peers.items() if uid not in (to_uid, self.uid) and is_online(uid)]
    random.shuffle(cands)                         # 随机顺序
    for p in cands:
        store_id = _new_msg_id()
        key      = secrets.token_bytes(32)        # 32 字节随机密钥
        rec = {"to_uid": to_uid, "from_uid": self.uid, "from_name": self.my_name,
               "blob": secret_encrypt(text, key),  # 十六进制密文串
               "key": key.hex(),                   # 十六进制密钥（明文随包传给接收方）
               "sig": secret_sign(key, to_uid, self.uid, store_id, text),
               "ts": now_ts(), "retries": 0}
        if _store_secret_on(p, store_id, rec):
            _emit("dm_sent", {"to_uid","ok":True,"direct":False,
                              "relay_name": p["name"], "text":text, "ts":rec["ts"]})
            return
    _emit("dm_sent", {"to_uid","ok":False,"reason":"对方不在线，且没有其他在线同学可代为转存"})
```

**密钥/签名算法**（第 308–349 行）：

```
secret_encrypt(text, key):                       # SHA-512 计数器模式 XOR
    明文 = text.encode("utf-8")；counter = 0
    循环：block = SHA512(key ‖ counter.to_bytes(8, "big")); 逐字节 密文 += 明文[i] ^ block[j]
    返回 密文.hex()（小写十六进制字符串）

secret_decrypt(hex_blob, key): 同算法逆运算；UTF-8 解码失败 → ValueError("解密失败：密钥不正确或数据损坏")

secret_sign(key, *parts):                        # HMAC-SHA256
    h = HMAC(key, SHA256)
    for p in parts: h.update(str(p).encode("utf-8")); h.update(b"\x00")
    返回 h.hexdigest()
```

**存取拓扑**：发送方 A →（TCP 短连）→ 转存方 B；B 落盘为密文（`data/secret_relay.json`）；B 全程无 UI、无日志、无事件（`_on_secret_store` 只写盘）。

**送达（B 侧，触发于收到目标 presence）**：

```
_on_presence_hook_deliver(uid):          # 由 _on_presence 调用
    if uid in self._delivering: return   # 并发去重
    self._delivering.add(uid)
    起线程 secret-deliver 执行 _deliver_secrets_to(uid)

_deliver_secrets_to(uid):
    try:
        entries = [(sid, rec) for sid, rec in list(self._secrets.items()) if rec["to_uid"] == uid]
        if not entries: return
        p = self.peers.get(uid, {})
        for sid, rec in entries:
            ok = self._deliver_one(p, sid, rec)          # TCP secret → secret_ack
            加锁：
                if ok: self._secrets.pop(sid)
                else:
                    rec["retries"] += 1
                    if rec["retries"] > SECRET_RELAY_RETRY(3): self._secrets.pop(sid)
        self._save_secrets()
    finally: self._delivering.discard(uid)
```

**重试语义（精确）**：`retries` 初值 0；每失败一次 +1；当 `retries > 3` 时删除。即**最多尝试 4 次**（retries 由 0→1→2→3 的失败后仍在，第 4 次失败使 retries=4 而被删除）。
**重试的触发条件**：仅仅依靠「目标再次发 presence」。源码**没有**定时器或退避重试；若目标一直在线，转存方**不会**再尝试（`_on_presence_hook_deliver` 只在收到 presence 时调用，而 presence 每 8 秒来一次——所以事实上每次 presence 都会重试，直到成功或超限）。

**接收（C 侧，`_on_secret_deliver`，第 1066–1092 行）**：

```
sid = str(req["store_id"] or "")
if req["to_uid"] != self.uid: return False
if sid and sid in self._recv_secrets: return True          # 幂等回执
try:
    key  = bytes.fromhex(str(req["key"] or ""))
    text = secret_decrypt(str(req["blob"] or ""), key)
    sig_ok = hmac.compare_digest(str(req["sig"] or ""),
                secret_sign(key, req["to_uid"], req["from_uid"], sid, text))
except Exception: return False
if not sig_ok: return False
if sid:
    self._recv_secrets.add(sid)
    if len(self._recv_secrets) > 2000:
        self._recv_secrets = set(sorted(self._recv_secrets)[-2000:])   # 保留字典序最大 2000 个
    self._save_recv_secrets()
_emit("dm", {"to_uid":self.uid, "from_uid":req["from_uid"], "from_name":req["from_name"] or "?",
             "text":text, "ts":req["ts"], "self":False, "relayed":True})
return True
```

**存转存（B 侧，`_on_secret_store`，第 1051–1064 行）**：

```
sid = str(req["store_id"] or "")
if not sid or not req["to_uid"] or not req["blob"] or not req["key"]: return   # 静默拒绝
if sid in self._secrets: return                                               # 重复存不覆盖
self._secrets[sid] = {"to_uid": str(req["to_uid"]), "from_uid": str(req["from_uid"] or ""),
                      "from_name": str(req["from_name"] or ""), "blob": str(req["blob"]),
                      "key": str(req["key"]), "sig": str(req["sig"] or ""),
                      "ts": req.get("ts", now_ts()), "retries": 0}
self._save_secrets()
```

> 安全提示（移植时保持行为即可）：`key` 与 `blob` 同包明文传输，转存方 B 能解密（只是不显示）。`_on_secret_deliver` 成功后**不删除**自己的 `_secrets`（该记录本就不在本机，属于转存方 B 的职责）。

### 8.6 内网外的消息一律忽略（`_member_ok`，第 1341–1348 行）

```
if not self.in_net(): return True        # 尚未加入内网时不限制（保持旧行为）
uid = str(uid or "")
if not uid: return True
return uid == self.uid or uid in self.net_members
```

调用点：`_on_chat`、`_on_dm`、`_on_log`、`_on_rename_req`、`_on_file_pub`、`_on_sync_req`。
**注意**：`_on_net_join_req` / `_on_net_join_vote` / `_on_net_registry` 等内网报文**不走** `_member_ok`，而是自己判断 `nid`。

`_require_net()`（第 1095–1098 行）：`not in_net()` 时抛 `PermissionError("还没有加入内网：请先在「内网」里申请加入或自行建立一个内网")`。
调用点：`send_chat`、`create_group`、`join_group`、`send_file_to_group`。

---

## 9. 移植风险最高的 3 点

1. **线程与并发模型（最大风险）**
   - `_broadcast` 从多个线程（presence、net、rename、GUI 线程）共用一个 `self._udp_sock` 发送，**无锁**；`_msg_counter += 1` 也**无锁**，`_new_msg_id()` 在并发下可能产生重复 ID，进而被 `_seen` 误去重。
   - `_threads` 列表被多个线程 append，`_delivering` / `_my_votes` / `_recv_secrets` 的读写混用 `self._lock` 与裸访问（如 `_on_rename_req` 直接操作 `_rename_requests`，`_deliver_secrets_to` 在锁外遍历 `_secrets`）。
   - `stop()` 只关 UDP socket，**不关 TCP 监听 socket 且不设 accept 超时**，`_tcp_loop` 可能永久阻塞在 `accept()` 上；`stop()` 回退为 `join(timeout=1.5)` 后线程仍存活。C++ 侧必须显式 shutdown/close 并处理 `_running` 与阻塞调用的交互，否则退出会挂死或崩溃。
   - `_safe` 包裹所有线程入口，异常仅打印堆栈；C++ 需要等价地保证「任何单线程异常都不终止进程」。

2. **JSON 数字/类型与 Python 动态类型的隐式容错**
   - 源码大量使用 `msg.get("x") or 默认值`、`int(...)`、`bool(...)`，对缺失/`null`/错误类型都能容错（例如 `approve` 用 `null`/`true`/`false` 三态，`_on_net_join_vote` 里 `None if approve is None else bool(approve)` 与非 0 整数 1/0 混用）。C++ 的强类型 JSON 解析必须逐个复刻这些回退规则（含 `_on_presence` 的 `ip or addr[0]`、`int(msg.get("port") or 0)`、`sorted(set(...) | {...})` 的集合语义）。
   - `votes`/`my_votes` 里 `approve` 的三态（`true` / `false` / `null`）与「键存在与否」在 `_req_progress` 里语义不同（`abstains` 要求 `m in votes`），用 C++ `bool` 或 `optional<bool>` 时极易写错。
   - `sort` 语义：`net_nearby_list` 按 `(-member_count, nid)` 排，而 `_net_bootstrap` 用的 `net_scan` 返回值**未排序**（字典插入序）；`sorted(set(voters)|{mac})[0]` 决定谁广播拒绝结果；`macs[:64]`、`reqs[-30:]`、`sorted(_recv_secrets)[-2000:]` 都依赖**字符串字典序**与切片语义，C++ 必须用同一比较器（Python 字符串比较是码点序，且全大写十六进制 ASCII 下与字节序一致）。

3. **加密/签名与「半可靠」送达协议的精确复刻**
   - `secret_encrypt` 是自制的 **SHA-512 计数器模式 XOR**：密钥流块为 `SHA512(key ‖ counter_big_endian_8B)`，`counter` 从 0 递增，**不以 64 字节为单位截断尾部**；`blob` 为小写十六进制字符串。任何一处（大端 8 字节、counter 起点、hex 大小写、UTF-8 编码时机）不一致，跨语言互通立即失败且**没有版本协商可纠错**。
   - `secret_sign` 是 `HMAC-SHA256(key, Σ str(part).encode("utf-8") + b"\x00")`，其中 `str(part)` 的 Python 字符串化规则（float `ts`、`bool`、set 等）必须一致；签名参与项为 `(to_uid, from_uid, store_id, text)`，其中 `text` 是**解密后的明文**——即接收方先解密再验签，失败时 `hmac.compare_digest` 返回 False。
   - 转存是无确认-重放式的「尽力送达」：TCP `secret_store` 的应答 `secret_stored` **无论校验成败都返回**（存失败也回成功），只有 `secret` 的 `secret_ack.ok` 才表示真实结果；重试计数 `retries > 3` 的边界（最多 4 次尝试）与「只在收到 presence 时重试」的触发条件必须照搬，否则会出现消息丢失或永久重试。
   - 另有内网准入投票的「票可改（按 ts 新胜旧）」与改名投票的「票不可改（uid 去重即弃）」两套相反语义，且入网结果广播只由 `sorted(voters ∪ {申请者})[0]` 发起——这三点是最容易在移植时被"顺手统一"而破坏协议的点。
