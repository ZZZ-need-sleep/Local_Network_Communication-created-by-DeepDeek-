# 02 · 群文件传输 与 内网准入 —— 精确协议规格

> 本文件是 `neiwang.py` 中 `class PeerCore`（源码第 567–2480 行）的**文件传输**与**内网准入**两部分的逐字复刻规格，供 C++ 重写使用。
> 一切以源码为准；源码没有定义的行为统一标注 **「源码未定义」**。
> **不要修改 `neiwang.py`；本规格不包含也不要求 C++ 代码。**

---

## 0. 覆盖的源码范围（已逐行核对）

| 源码行 | 内容 |
| --- | --- |
| 61–135 | 全局常量（含全部 `NET_*` 与文件相关常量） |
| 161–199 | `sanitize_name`、`fmt_mac`、`net_vote_need` |
| 289–302 | `compute_sha256`（文件指纹算法） |
| 459–482 | TCP 帧编解码 `encode_frame` / `recv_exact` / `send_json` / `recv_json` |
| 567–624 | `PeerCore.__init__` 状态字段初始化（`net`、`net_members`、`net_reqs`、`net_nearby`、`net_my_votes`、`net_applying`、`index`、`_offers`、`_downloading`、`_download_queue`、`_active_downloads`） |
| 633–649 | `start()` 启动的 6 个线程（含 `download`、`net`） |
| 672–676 | `_emit` 事件投递 |
| 712–738 | `_broadcast` 报文封装与广播地址 |
| 740–809 | UDP 收包主循环与 `t` 分发表、去重 |
| 848–850, 1095–1098, 1341–1348 | `is_online` / `_require_net` / `_member_ok`（内网准入闸门） |
| 1286–1296 | `log` 广播 |
| 1298–2035 | **内网准入全量**：`net.json` 读写、查询、建网/退网/改名、扫描/申请、投票、广播、收包、审批评估、周期循环 |
| 2036–2441 | **文件全量**：目录与索引、发送广播、收到公告、自动下载、公共询问回退、分块 TCP 下载、TCP 服务端 |
| 3731–3792 | GUI 事件轮询（事件 kind → 载荷的权威对照） |
| 3393–3484, 3584–3617, 3846–3873, 4080–4185 | GUI 侧内网操作 / 群文件视图 / 重试下载（用于确认对外可见字段） |

**未覆盖（本规格范围外）**：第 944–1093 行保密转存（`secret_store`/`secret` 两个 TCP 命令、`dm`/`dm_sent` 事件）、第 1141–1284 行改名投票、第 2443–2479 行检查更新。这三块与文件/内网无耦合，可另行规格化。

`class PeerCore` 实际结束于第 2480 行（第 2489 行起是 `class P` 调色板）。

---

## 1. 通用约定（所有报文共用）

### 1.1 身份

* `uid`：本机唯一 ID。取值优先级：真实网卡 MAC（12 位大写十六进制，可含 A–F，见 `make_uid`/`uuid_getnode_mac`）→ 取不到时 `"RND" + hex(6字节).upper()`（长度 15，形如 `RND1A2B3C4D5E6F`）。
* **`uid` 同时充当「物理地址（MAC）」，也就是 `net_members` 的键。** 内网一切「物理地址」字样都是 `uid`。C++ 侧必须把「设备 ID」与「成员表键」实现为同一个值。
* `name`：显示名，默认 `socket.gethostname()`，可被改名投票流程改写（`config.json` 的 `name`）。

### 1.2 传输层

| 项 | 值 |
| --- | --- |
| 发现/控制 | UDP 广播，单包（`recvfrom(65536)`） |
| UDP 端口 | `config.json.udp_port`，默认 `DEFAULT_UDP_PORT = 45455`（env `NEIWANG_UDP_PORT` 覆盖）。绑定失败时绑 0（随机口）。 |
| TCP 端口 | `config.json.tcp_port`，默认 `DEFAULT_TCP_PORT = 45431`（env `NEIWANG_TCP_PORT`）。绑定失败时依次试 `port+1 … port+20`，成功即写回 `config.json`；全失败抛 `RuntimeError("无法绑定 TCP 端口")`。 |
| UDP socket 选项 | `SO_REUSEADDR=1`、`SO_BROADCAST=1`、超时 1.0 秒 |
| TCP listen backlog | 16 |

**广播目标集合**（`_broadcast`，`addr` 为空时）：`255.255.255.255`、每一张本机网卡的 `x.y.z.255` 定向广播、以及每张网卡的自身 IP；全部发往 `udp_port`。单播（`addr` 非空）时只发该地址。

### 1.3 报文信封（`_broadcast` 自动补齐）

每条 UDP 报文的 JSON **顶层** 都会通过 `setdefault` 补齐以下三个键（呼叫方已给则不覆盖）：

| key | 类型 | 含义 |
| --- | --- | --- |
| `uid` | string | 发送方设备 ID |
| `name` | string | 发送方显示名 |
| `msg_id` | string | 报文唯一 ID，格式 `"{uid}-{自增计数}-{6位十六进制随机}"` |

**报文的「类型」字段名是 `t`（不是 `type`）。源码全文没有出现 `type` 作为报文键。** C++ 必须用 `t`。

### 1.4 收包处理顺序（`_udp_loop` + `_dispatch_udp`）

1. `recvfrom(65536)`；`socket.timeout` → 继续；`OSError` → 退出循环（线程结束）。
2. UTF-8 解码 + JSON 解析；失败 → 丢弃。
3. 非 JSON object → 丢弃。
4. **`msg["uid"] == self.uid` → 直接丢弃**（自己的广播不处理）。
5. 去重：若 `msg_id` 非空——
   * 已存在于 `_seen` → 丢弃；
   * 否则 `_seen[msg_id] = now`，若 `len(_seen) > 4000` 则先按 `SEEN_TTL = 120` 秒清理。
6. 按 `t` 分发；**未列出的 `t` 静默忽略**，不记日志。

分发表（内网 + 文件，本规格相关）：

| `t` | 处理函数 |
| --- | --- |
| `presence` | `_on_presence` |
| `chat` | `_on_chat` |
| `file_pub` | `_on_file_pub` |
| `file_req` | `_on_file_req` |
| `file_have` | `_on_file_have` |
| `file_sync_req` | `_on_sync_req` |
| `log` | `_on_log` |
| `group_announce` | `_on_group_announce` |
| `rename_req` / `rename_vote` | （改名，范围外） |
| `dm` | （私聊，范围外） |
| `net_scan` | `_on_net_scan` |
| `net_announce` | `_on_net_announce` |
| `net_join_req` | `_on_net_join_req` |
| `net_join_vote` | `_on_net_join_vote` |
| `net_registry` | `_on_net_registry` |
| `net_member_left` | `_on_net_member_left` |
| `net_join_result` | `_on_net_join_result` |

### 1.5 内网准入闸门

* `in_net()` = `self.net is not None and self.net["nid"]` 非空。
* `_require_net()`：不在内网 → 抛 `PermissionError("还没有加入内网：请先在「内网」里申请加入或自行建立一个内网")`。被 `create_group`、`join_group`、`send_chat`、`send_file_to_group` 调用（GUI 另有前置判断）。
* `_member_ok(uid)`：
  * 自己不在内网 → **返回 True（不限制，源码注释「保持旧行为」）**；
  * `uid` 为空 → True；
  * 否则 `uid == self.uid or uid in self.net_members`。
* 被 `_member_ok` 拦截的入口：`_on_chat`、`_on_dm`、`_on_log`、`_on_rename_req`、`_on_file_pub`、`_on_sync_req`。
  **注意：`_on_file_req`、`_on_file_have`、`_on_net_join_req`、`_on_net_scan` 等不做 `_member_ok` 检查**，只做「自己是否在内网 / nid 是否匹配」判断。

---

## 2. 常量总表

### 2.1 内网（`NET_*`）

| 常量名 | 默认值 | 环境变量覆盖 | 含义 |
| --- | --- | --- | --- |
| `NET_LIMIT_NO_VOTE` | `5` (int) | `NEIWANG_NET_NO_VOTE` | 成员数 ≤ 该值 → 免投票自动通过 |
| `NET_LIMIT_FULL` | `7` (int) | `NEIWANG_NET_FULL_VOTE` | 成员数 ≤ 该值（且 > 上一个）→ 全票通过 |
| `NET_SCAN_WINDOW` | `3.0` (float 秒) | `NEIWANG_SCAN_WINDOW` | 启动/手动扫描附近内网的时长 |
| `NET_VOTE_WINDOW` | `86400.0` (float 秒＝1 天) | `NEIWANG_NET_VOTE_WINDOW` | 入网投票有效期；也用作申请 `deadline` 增量 |
| `NET_VOTE_CHANGE` | `300.0` (float 秒＝5 分钟) | `NEIWANG_NET_VOTE_CHANGE` | 投出后允许改票的窗口 |
| `NET_ANNOUNCE_INTERVAL` | `15.0` (float 秒) | `NEIWANG_NET_ANNOUNCE` | 内网公告 + 登记表同步周期 |
| `NET_REQ_REBROADCAST` | `10.0` (float 秒) | `NEIWANG_NET_REQ_REBROADCAST` | 申请人重播 `net_join_req` 的周期 |
| `NET_NEARBY_TTL` | `40.0` (float 秒) | `NEIWANG_NET_NEARBY_TTL` | `net_nearby` 条目有效期 |
| `NET_NO_MEMBER` | `"（不属于本内网）"` | — | **源码中定义但从未被引用**（C++ 可保留常量或省略，行为上无影响） |

### 2.2 文件与通用

| 常量名 | 默认值 | 含义 |
| --- | --- | --- |
| `FILE_CHUNK` | `128 * 1024` = `131072` 字节 | 文件读写分块大小（哈希、TCP 收发共用） |
| `OFFER_WAIT` | `8.0` 秒（env `NEIWANG_OFFER_WAIT`） | 公共询问 `file_req` 后等待应答的时间，也是每次回退尝试的等待时长 |
| `FALLBACK_RETRY` | `2`（int） | 公共询问回退的额外重试次数上限（总尝试 = 1 + 2 = 3 次） |
| `MAX_CHAT_LEN` | `4000` | 聊天/私聊文本截断长度（与本规格无关但常被一起提到） |
| `SEEN_TTL` | `120` 秒 | UDP 去重窗口 |
| `PRESENCE_INTERVAL` | `8.0` 秒 | `presence` 广播周期 |
| `PRESENCE_TTL` | `30.0` 秒 | 超过即视为离线（`is_online` 用） |
| `LOG_MAX_LINES` | `800` | 仅 GUI 日志控件行数上限 |
| `SECRET_RELAY_RETRY` | `3` | 保密转存重试（范围外） |
| `RENAME_VOTE_NEED` / `RENAME_VOTE_WAIT` | `5` / `120.0` | 改名投票（范围外） |

`net_vote_need(members) -> (need, desc)` 的准确算法见 §7.3。

---

## 3. 文件相关 UDP 报文（逐个字段）

约定：下表的「信封」三键（`uid`/`name`/`msg_id`）总是存在，不再重复列出。类型 `null` 表示 JSON `null`。

### 3.1 `file_pub` —— 群文件公告（发送 / 重播）

发送时机：
* `send_file_to_group(path, gid)` 计算完指纹并入库后立即广播（`resync` 缺省不存在）；
* `_on_sync_req` 收到 `file_sync_req` 时，对每个「属于该群且本机有实体文件」的索引记录各广播一次，带 `resync: true`，且 `sender_uid`/`sender_name` **沿用索引里的原始发送人**（不是转发者）。

| key | 类型 | 必须 | 含义 / 默认 |
| --- | --- | --- | --- |
| `t` | string | 是 | 字面量 `"file_pub"` |
| `gid` | string | 是 | 群号（大写 hex） |
| `gname` | string | 是（发送时）；重播时取 `msg.gname`，可为 `""` | 群名 |
| `file` | object | 是 | 见下 |
| `file.id` | string | 是 | 文件 ID = 文件内容的 SHA-256 小写十六进制（64 字符） |
| `file.name` | string | 是 | 原始文件名（`path.name`，发送时未做 `sanitize_name`） |
| `file.size` | int | 是 | 字节数 |
| `ts` | float | 是 | 发送时间戳（重播时是新时间戳） |
| `sender_uid` | string | 是 | 原始发送人设备 ID |
| `sender_name` | string | 是 | 原始发送人显示名 |
| `resync` | bool | 否 | 仅重播时出现，值 `true`。**接收方完全不读该字段**（仅作人读标记） |

接收方 `_on_file_pub(msg)` 行为：
1. `gid` 不在 `self.groups` → 忽略。
2. `not _member_ok(msg.uid)` → 忽略。
3. `file.id` 为空 → 忽略。
4. `gname` 取 `msg.gname`，缺失时回落 `self.groups[gid].gname`。
5. `sender = msg.sender_name or msg.name or "?"`；`sender_uid = msg.sender_uid or msg.uid`。
6. **无条件**向公共日志推一条 `log` 事件（即使本机已有该文件）。
7. 推 `file_new` 事件（`from_self=false`），其中内嵌 `file` 视图的 `has_local` 用 `has_file(fid)` 实时计算。
8. 若 `has_file(fid)` → 结束。
9. 若 `fid in self._downloading or fid in self._active_downloads` → 结束。
10. 否则把任务压入 `_download_queue`：`{"fid","name","size","gid","gname","sender_uid","sender_name"}`（无 `attempt` 键，默认 0）。

> 「重播」不存在去重保护：同一个 `file_pub` 会被多个持有者重复广播，接收方靠 `has_file`/下载中集合兜底。

### 3.2 `file_sync_req` —— 群文件列表同步请求

发送时机：`_sync_req(gid)`，被 `create_group`、`join_group` 调用（加入群组后立即请求补收历史文件）。

| key | 类型 | 必须 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 是 | `"file_sync_req"` |
| `gid` | string | 是 | 群号 |
| `gname` | string | 是 | 群名，取 `self.groups[gid].gname`，群不存在时 `""` |

接收方 `_on_sync_req(msg)`：
1. `gid` 为空 → 忽略。
2. `not _member_ok(msg.uid)` → 忽略。
3. 遍历 `self.index`，对满足 `gid in rec.groups and has_file(fid)` 的每条，广播一条 `file_pub`（见 3.1，带 `resync:true`）。
4. **没有应答报文**，全部以 `file_pub` 形式回复。

### 3.3 `file_req` —— 发送人离线时的公共询问

发送时机：`_public_request(task, req_id)`，在直连失败/发送人离线后调用，**广播**给全网（所有内网成员可见）。

| key | 类型 | 必须 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 是 | `"file_req"` |
| `req_id` | string | 是 | 询问唯一 ID，由 `_new_msg_id()` 生成 |
| `gid` | string | 是 | 群号 |
| `gname` | string | 是 | 群名 |
| `sender_uid` | string | 是 | 原始发送人 ID（可为 `""`） |
| `sender_name` | string | 是 | 原始发送人名字 |
| `file` | object | 是 | `{"id": fid, "name": name, "size": size}` |
| `ts` | float | 是 | 时间戳 |

发送方同时把这条询问写进自己的日志：`"[MM-DD HH:MM:SS] {my_name}: 公共询问文件「{name}」（发送人 {sender_name}，群「{gname}」）"`。

接收方 `_on_file_req(msg, addr)`：
1. `file.id` 或 `req_id` 为空 → 忽略。
2. 无条件写日志：`"[..] {msg.name}: 公共询问文件「{file.name}」（发送人 {msg.sender_name}，群「{msg.gname}」）"`（缺失字段显示 `"?"`）。
3. **若 `has_file(fid)`** → 广播 `file_have`；否则**完全无应答**（这是「没下载过的无应答」的准确实现）。
4. 不做 `_member_ok` 检查。

### 3.4 `file_have` —— 「我有这个文件」应答

发送时机：`_on_file_req` 中本机持有该文件时 **广播**（不是单播）。

| key | 类型 | 必须 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 是 | `"file_have"` |
| `req_id` | string | 是 | 回抄询问的 `req_id` |
| `file_id` | string | 是 | 文件 ID（= 请求者的 `file.id`） |
| `ip` | string | 是 | **应答者第一张本机网卡 IP**：`get_local_ips()[0]` |
| `port` | int | 是 | 应答者当前 TCP 端口 `my_tcp_port()` |
| `ts` | float | 是 | 时间戳 |

接收方 `_on_file_have(msg)`：仅当 `req_id in self._offers` 时，把元组 `(msg.ip, int(msg.port or 0), msg.uid)` 追加到 `self._offers[req_id]` 列表尾部。**先到先得由列表顺序决定**（源码注释：「向第一个返回的 IP 自动请求文件」）。

### 3.5 其它相关报文（简）

| `t` | 字段 | 说明 |
| --- | --- | --- |
| `log` | `text`(string ≤300)、`ts`(float) | `broadcast_log` 发送；接收方需 `_member_ok` |
| `group_announce` | `gid`、`gname` | 建群时广播；接收方记录 `discovered[gid]` |
| `presence` | `ip`(string, `get_local_ips()[0]`)、`port`(int, `my_tcp_port()`)、`groups`(array of `{gid,gname}`)、`ver`(string, 常量 `APP_VERSION`) | 每 8 秒广播；接收方写入 `self.peers[uid] = {name, ip, tcp_port, groups, ver, ts}`，其中 `ip` 取 `msg.ip or addr[0]`、`tcp_port` 取 `int(msg.port or 0)`。**`is_online(uid)` 要求 `tcp_port != 0`。** |

---

## 4. 内网相关 UDP 报文（逐个字段）

### 4.1 `net_scan` —— 扫描附近内网

发送时机：`net_scan(seconds)` 循环内**每 1.0 秒**广播一次，直到 `deadline = now + max(0.5, seconds)`。

| key | 类型 | 必须 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 是 | `"net_scan"` |
| `ts` | float | 是 | 时间戳 |

接收方 `_on_net_scan(msg, addr)`：
1. 自己不在内网 → 忽略。
2. **应答节流**：`now - self._net_reply_ts < 0.5` → 忽略；否则更新 `_net_reply_ts = now`。
3. 调用 `_net_announce()` —— **用广播应答，不用单播**（源码注释：同机多实例共用 UDP 端口时单播只会被其中一个 socket 收到）。

### 4.2 `net_announce` —— 内网公告（扫描应答 / 周期公告）

发送时机：`_net_announce(addr)`，被 `net_create`、`net_rename`、`_on_net_scan`、`_net_join_success`、`_net_bootstrap`、`_net_tick`(每 `NET_ANNOUNCE_INTERVAL`) 调用；`addr` 恒为 `None`（全广播）。

| key | 类型 | 必须 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 是 | `"net_announce"` |
| `nid` | string | 是 | 内网 ID（6 字节 hex 大写，`secrets.token_hex(3).upper()`） |
| `net_name` | string | 是 | 内网名 |
| `founder_mac` | string | 是 | 发起者设备 ID（可能是 `""`） |
| `member_count` | int | 是 | `len(self.net_members)` |
| `macs` | array[string] | 是 | `sorted(self.net_members)[:64]` —— 成员设备 ID **按键名字典序排序后取前 64 个** |
| `ts` | float | 是 | 时间戳 |

接收方 `_on_net_announce(msg, addr)`：
1. `nid` 为空 → 忽略。
2. 自己已在该内网（`in_net() and nid == self.net.nid`）→ 忽略。
3. 写入/覆盖 `self.net_nearby[nid] = {"nid", "name"(缺省 `"内网{nid[:6]}"`), "founder_mac", "member_count"(int), "macs"(array), "ip"(= `addr[0]`, 无 addr 时 `""`), "ts": now}`。
4. 推 `net_nearby_changed` 事件。

`net_nearby_list()` 排序：`key = (-member_count, nid)`。

### 4.3 `net_join_req` —— 入网申请

发送时机：`net_apply_join` 立即发一次，随后 `_net_tick` 每 `NET_REQ_REBROADCAST = 10` 秒重播，直到申请结束；`_net_bootstrap` 若发现上次遗留的 `net_applying` 也会立刻重播（`_net_last_req` 复位为 0）。

| key | 类型 | 必须 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 是 | `"net_join_req"` |
| `req_id` | string | 是 | 申请 ID，`_new_msg_id()` |
| `nid` | string | 是 | 目标内网 ID |
| `mac` | string | 是 | **申请人的 `uid`（即物理地址）** |
| `name` | string | 是 | 申请人显示名 |
| `ip` | string | 是 | `get_local_ips()[0]` |
| `tcp_port` | int | 是 | `my_tcp_port()` |
| `voters` | array[string] | 是 | 申请人从扫描结果推定的投票人名单（见 §7.4） |
| `created_ts` | float | 是 | 申请创建时间（重播时不变） |
| `ts` | float | 是 | 本次发送时间 |

接收方 `_on_net_join_req(msg, addr)`：
1. 自己不在内网 → 忽略。
2. `msg.nid != self.net.nid` → 忽略。
3. `req_id` 或 `mac` 为空 → 忽略。
4. **`mac` 已是成员** → 只调用 `_net_sync()`（把登记表广播出去，让申请人直接完成加入），然后返回，**不建申请记录**。
5. 若本地已有该 `req_id` 且状态非 `pending` → 忽略。
6. 全新申请时建记录：
   ```
   {
     "req_id", "nid", "mac",
     "name": msg.name or "?",
     "ip": msg.ip or (addr[0] if addr else ""),
     "tcp_port": int(msg.tcp_port or 0),
     "voters": sorted(set(msg.voters or []) | {self.uid}),
     "votes": {}, "state": "pending", "note": "",
     "created_ts": float(msg.created_ts or now),
     "deadline": created_ts + NET_VOTE_WINDOW
   }
   ```
   并推一条 `log` 事件：`"{name}（物理地址 {fmt_mac(mac)}）申请加入内网「{net_name}」：{desc}"`，再推 `net_vote_changed`。
7. 若已有该记录（重播）→ 合并投票人名单：`voters = sorted(set(local.voters) | set(msg.voters) | {self.uid})`。
8. 统一：`_save_net()` → `_net_eval_req(req)` → `_net_sync()`。

### 4.4 `net_join_vote` —— 入网投票

发送时机：`net_cast_vote` 立即发；`_net_tick` 每 60 秒对所有仍 `pending` 的、我投过票的申请**重播原票**（`ts` 用当初投票的时间戳）。

| key | 类型 | 必须 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 是 | `"net_join_vote"` |
| `req_id` | string | 是 | 申请 ID |
| `nid` | string | 是 | 内网 ID |
| `voter` | string | 是 | 投票人设备 ID（= 投票人的 `uid`） |
| `voter_name` | string | 是 | 投票人显示名 |
| `approve` | bool \| null | 是 | `true` 同意、`false` 拒绝、`null` 弃权 |
| `ts` | float | 是 | 投票时间戳（改票时更新；重播时沿用原值） |

接收方 `_on_net_join_vote(msg)`：
1. `req_id` 无对应记录或状态非 `pending` → 忽略。
2. `voter = msg.voter or msg.uid`；为空或不在 `req.voters` 里 → 忽略（**非投票人不能投**）。
3. `approve`：`None` 保持 `None`，否则 `bool(...)`；`ts = float(msg.ts or now)`。
4. **旧票忽略**：若本地已有该 voter 的票且 `old.ts >= ts` → 忽略（保证重播/乱序不会覆盖新票）。
5. 否则写 `req.votes[voter] = {"approve": approve, "ts": ts, "name": voter_name}`；`_save_net()`；推 `net_vote_changed`；`_net_eval_req(req)`。

### 4.5 `net_registry` —— 内网登记表同步（成员表 + 申请 + 投票）

发送时机：`_net_sync()`，被 `net_create`、`net_rename`、`net_cast_vote`、`_on_net_join_req`、`_net_approve_req`、`_net_join_success`、`_net_bootstrap`、`_net_tick`(每 `NET_ANNOUNCE_INTERVAL`) 调用；`addr` 恒为 `None`（全广播）。

`reqs` 的筛选与截断（**源码精确逻辑**）：
```
reqs = [r for r in net_reqs.values()
        if r.nid == net.nid and (r.state == "pending"
                                 or now - float(r.get("closed_ts") or 0) <= 3600)]
reqs = reqs[-30:]     # 取列表尾部最多 30 条（依赖 dict 插入顺序）
```

| key | 类型 | 必须 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 是 | `"net_registry"` |
| `nid` | string | 是 | 内网 ID |
| `net_name` | string | 是 | 内网名 |
| `founder_mac` | string | 是 | 发起者设备 ID |
| `members` | array[object] | 是 | 成员表原样，元素结构见 §5.2 |
| `reqs` | array[object] | 是 | 申请/投票记录原样，元素结构见 §5.3（含 `votes` 明细） |
| `ts` | float | 是 | 时间戳 |

接收方 `_on_net_registry(msg, addr)` 分两支：

**A. 我已经在这个内网里**（`nid == self.net.nid`）：
1. 对 `members` 每项调用 `_net_add_member(mac, name, ip, tcp_port, founder)`，任一返回 true 则 `changed = true`。
2. 对 `reqs` 每项调用 `_merge_req(r)`，任一返回 true 则 `changed = true`。
3. 对本地所有 `state == "pending"` 且 `nid` 匹配的申请调用 `_net_eval_req(r)`。
4. `changed` 则 `_save_net()`。
5. **无条件**推 `net_changed` 与 `net_vote_changed`。

**B. 我还没入网**：
1. 若没有 `net_applying` 或 `nid != applying.nid` → 忽略。
2. 若 `self.uid` 出现在 `members[].mac` 中 → 调 `_net_join_success(msg)` 并返回（**这是「申请被通过」的唯一判定通道**）。
3. 否则在 `reqs` 里找 `req_id == applying.req_id`：
   * 把远端 `votes` 里 `ts` 更大的票合并进本地申请记录的 `votes`；
   * 合并 `voters` 并集；
   * 若远端 `state == "rejected"` → 本地申请记录置 `state="rejected"`、`note=远端 note`，记下 `rejected` 文案。
4. 若有 `rejected` → `net_applying = None`、`_save_net()`、推 `net_result(False, "入网申请未通过：{note}")` + `net_vote_changed` + `net_changed`，返回。
5. 否则若有 `changed` → 推 `net_vote_changed` + `net_changed`。

`_merge_req(remote) -> bool`（本地已有/新建申请记录的合并规则）：
* 无 `req_id` → 返回 false。
* 本地无该记录 → 新建：字段同 §5.3，其中 `state` 取 `remote.state`（默认 `"pending"`）、`deadline = float(remote.deadline or (created_ts + NET_VOTE_WINDOW))`，返回 true。
* 本地有：合并 `voters` 并集；对远端每张票，`ts` 更大才覆盖；**仅当** 本地 `state == "pending"` 且远端 `state in ("approved","rejected")` 时同步状态与 `note`。

### 4.6 `net_member_left` —— 成员退出通知

发送时机：`net_leave()` 广播一次。

| key | 类型 | 必须 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 是 | `"net_member_left"` |
| `nid` | string | 是 | 内网 ID |
| `mac` | string | 是 | 退出者 `uid` |
| `name` | string | 是 | 退出者显示名 |
| `ts` | — | **无** | 该报文**没有 `ts` 字段**（源码如此） |

接收方 `_on_net_member_left(msg)`：自己不在内网或 nid 不匹配 → 忽略；`mac` 非空且能从 `net_members` 中 `pop` 掉 → `_save_net()`、写日志 `"{name}（物理地址 {MAC}）退出了内网"`、推 `net_changed`。

### 4.7 `net_join_result` —— 申请被拒绝的结果广播

发送时机：`_net_reject_req(req, note)` 中，**只有当 `sorted(set(req.voters) | {req.mac})[0] == self.uid` 时**才广播（物理地址字典序最小者统一宣布，避免重复）。

| key | 类型 | 必须 | 含义 |
| --- | --- | --- | --- |
| `t` | string | 是 | `"net_join_result"` |
| `req_id` | string | 是 | 申请 ID |
| `nid` | string | 是 | 内网 ID |
| `mac` | string | 是 | 申请人设备 ID |
| `approved` | bool | 是 | 恒为 `false`（通过不广播此报文） |
| `reason` | string | 是 | 拒绝原因（= `note`） |
| `ts` | float | 是 | 时间戳 |

接收方 `_on_net_join_result(msg)`：
1. 没有 `net_applying` 或 `req_id` 不匹配 → 忽略。
2. `msg.approved` 为真 → 直接返回（**通过路径不走这里**）。
3. `reason = msg.reason or "申请未通过"`；`net_applying = None`；本地申请记录置 `state="rejected"`、`note=reason`；`_save_net()`；推 `net_result(False, "入网申请未通过：{reason}")` + `net_vote_changed` + `net_changed`。

> **重要缺口**：没有「已投过申请不通过」的单播确认，批准也没有专门的 `net_join_result`；批准完全依赖 `net_registry` 里出现自己的 `mac`。C++ 复刻时必须保持这一点。

---

## 5. `data/` 目录：文件与 JSON 结构

### 5.1 路径总览

数据根目录 `DATA_DIR`：
* 若设置环境变量 `NEIWANG_DATA_DIR` → 用它；
* 否则 `Path(neiwang.py).resolve().parent / "data"`；
* 目录不存在则 `mkdir(parents=True, exist_ok=True)`。

`PeerCore` 内部用 `self.data_root`（构造参数 `data_root` 可覆盖，默认 `DATA_DIR`；测试里用于隔离）。下面「`data/`」即 `data_root`。

```
data/
├─ config.json          # 全局配置（load_config/save_config，indent=2）
├─ index.json           # 文件索引（indent=1）        ← 本规格
├─ net.json             # 内网状态（indent=1）        ← 本规格
├─ secret_relay.json    # 保密转存（范围外）
├─ secret_received.json # 已收转存 ID（范围外）
├─ cache/               # 内容寻址缓存 ← 本规格
│   ├─ <sha256hex>          # 完整文件（文件名即 64 位小写 hex）
│   └─ <sha256hex>.part     # 下载中的临时文件（失败时可能残留）
└─ downloads/           # 用户可见的下载目录 ← 本规格
    └─ <群名前30字>_<gid前6位>/
        └─ <文件名>
```

### 5.2 `net.json`

**写入时机**（`_save_net`，`ensure_ascii=False`，`indent=1`）：`_load_net` 之后的任何内网状态变更，包括 `net_create`、`net_leave`、`net_rename`、`net_cancel_apply`、`net_cast_vote`、`_on_net_join_req`、`_on_net_join_vote`、`_on_net_registry`（有变更时）、`_on_net_member_left`、`_net_approve_req`、`_net_reject_req`、`_net_join_success`、`_on_net_join_result`、`_net_tick`（申请超时分支）、`_net_prune`。写失败静默吞掉。

**读取时机**：`PeerCore.__init__` 末尾调用 `_load_net()` 一次（`start()` 之前）。

顶层结构：

```json
{
  "net": {
    "nid": "A1B2C3",
    "name": "高三（2）班内网",
    "founder_mac": "AABBCCDDEEFF",
    "created_ts": 1730000000.0,
    "joined_ts": 1730000100.0
  },
  "members": [
    {"mac": "AABBCCDDEEFF", "name": "AA", "ip": "192.168.1.10",
     "tcp_port": 45431, "joined_ts": 1730000000.0, "founder": true}
  ],
  "reqs": [
    {"req_id": "UID-12-ab12cd", "nid": "A1B2C3", "mac": "112233445566",
     "name": "X", "ip": "192.168.1.22", "tcp_port": 45431,
     "voters": ["AABBCCDDEEFF", "112233445566"],
     "votes": {"AABBCCDDEEFF": {"approve": true, "ts": 1730000200.0, "name": "AA"}},
     "state": "pending", "note": "", "created_ts": 1730000150.0,
     "deadline": 1730086550.0, "closed_ts": 1730000300.0}
  ],
  "my_votes": {"UID-12-ab12cd": {"approve": true, "ts": 1730000200.0}},
  "applying": {"req_id": "UID-12-ab12cd", "nid": "A1B2C3", "name": "内网名",
               "voters": ["AABBCCDDEEFF"], "created_ts": 1730000150.0,
               "deadline": 1730086550.0}
}
```

字段定义：

| 路径 | 类型 | 说明 |
| --- | --- | --- |
| `net` | object \| null | 我所在的内网；`null` 表示未加入 |
| `net.nid` | string | 内网 ID（6 字节 hex 大写）。**`net` 非 null 但 `nid` 为空 → 整个文件视为无效，不加载** |
| `net.name` | string | 内网名（`sanitize_name(name, 30)`，默认 `"{hostname} 的内网"`） |
| `net.founder_mac` | string | 发起者 `uid` |
| `net.created_ts` | float | 建网时间（`net_create` 写入；由申请加入时 `_net_join_success` 写的是**本机加入时刻**） |
| `net.joined_ts` | float | **仅** `_net_join_success` 写入；`net_create` 创建的 `net` 没有这个键。源码未在任何逻辑中读取它 |
| `members` | array | 登记表；加载时以 `m["mac"]` 为键建字典，跳过非 object 或无 `mac` 的项 |
| `members[].mac` | string | 设备 ID = `uid` |
| `members[].name` | string | 显示名，默认 `"?"` |
| `members[].ip` | string | 内网 IP，默认 `""` |
| `members[].tcp_port` | int | TCP 端口，默认 `0` |
| `members[].joined_ts` | float | 首次登记时间（**再次 `_net_add_member` 时不会更新**） |
| `members[].founder` | bool | 是否发起者 |
| `reqs` | array | 申请/投票记录；加载时以 `req_id` 为键 |
| `reqs[].req_id` | string | 申请 ID |
| `reqs[].nid` | string | 内网 ID |
| `reqs[].mac` | string | 申请人设备 ID |
| `reqs[].name` | string | 申请人名字 |
| `reqs[].ip` | string | 申请人 IP |
| `reqs[].tcp_port` | int | 申请人 TCP 端口 |
| `reqs[].voters` | array[string] | 拥有投票权的成员设备 ID 列表（**门槛按此列表长度算，不按成员总数**） |
| `reqs[].votes` | object | `设备ID → {"approve": bool\|null, "ts": float, "name": string}` |
| `reqs[].state` | string | `"pending"` \| `"approved"` \| `"rejected"` |
| `reqs[].note` | string | 结论说明（中文），默认 `""` |
| `reqs[].created_ts` | float | 申请创建时间 |
| `reqs[].deadline` | float | `created_ts + NET_VOTE_WINDOW` |
| `reqs[].closed_ts` | float | **仅审批结束时写入**（`_net_approve_req`/`_net_reject_req`）；`net_registry` 用它判断「最近 1 小时内结束的申请」 |
| `my_votes` | object | `req_id → {"approve": bool\|null, "ts": float}`，本机投出的票（用于改票窗口与重播） |
| `applying` | object \| null | 本机正在等待审批的申请（不含 `votes`，只有 `req_id`/`nid`/`name`/`voters`/`created_ts`/`deadline`） |

`_load_net` 的准确行为：
1. 文件不存在 → 直接返回（保持构造时的默认值）。
2. 解析 JSON；`net = d.get("net") or None`。
3. **只有 `net` 存在且 `net.nid` 非空**才继续：
   * `self.net = net`；
   * `members` / `reqs` / `my_votes` / `applying` 按上表装载（非 dict 的成员/申请项被跳过）；
   * **重新把本机登记进成员表**：`_net_add_member(self.uid, self.my_name, get_local_ips()[0], int(self.cfg.get("tcp_port") or 0))`——注意这里用的是 `config.json` 里的 `tcp_port`，**不是** `my_tcp_port()`（此时 TCP 服务尚未绑定）。
4. 任何异常 → `traceback.print_exc()`，状态保持构造默认值。

> **源码未定义的边界**：`applying` 的恢复被写在「`net.nid` 非空」分支内，而按语义 `applying` 与 `net` 互斥（申请中就不会有 `net`）。因此实际运行中 `applying` **不会被恢复**。C++ 若要忠实复刻，请照抄该嵌套结构（或明确选择修正，并记录差异）。

### 5.3 `index.json`（文件索引）

**写入时机**（`_save_index`，`ensure_ascii=False`，`indent=1`）：仅由 `_register_file(fid, rec)` 触发，即：
* `send_file_to_group` 计算指纹、复制进缓存之后；
* `_finish_download` 下载成功并拷贝到 downloads 之后。

**读取时机**：`PeerCore.__init__` 中 `_load_index()`（早于 `_load_net`）。文件不存在或解析失败 → `self.index = {}`。

结构：**顶层是一个以文件 ID 为键的对象（map），不是数组**。

```json
{
  "3f786850e387550fdab836ed7e6dc881de23001b...": {
    "name": "你好.txt",
    "size": 12345,
    "ts": 1730000000.0,
    "src_uid": "AABBCCDDEEFF",
    "src_name": "AA",
    "groups": ["A1B2C3"],
    "has_local": true,
    "local_path": "E:\\...\\data\\downloads\\高三（2）班_A1B2C3\\你好.txt"
  }
}
```

| key | 类型 | 说明 |
| --- | --- | --- |
| （顶层键） | string | 文件 ID = SHA-256 小写 hex（64 字符） |
| `name` | string | 文件名 |
| `size` | int | 字节数 |
| `ts` | float | 记录时间戳 |
| `src_uid` | string | 原始发送人 `uid` |
| `src_name` | string | 原始发送人名字 |
| `groups` | array[string] | 所属群号列表（`_register_file` 会与旧值取并集并排序） |
| `has_local` | bool | 本机是否持有实体文件（真值判定还要看 `cache/<fid>` 是否存在） |
| `local_path` | string | **可选**，仅 `_finish_download` 写入，指向 downloads 里的友好文件名路径 |

`_register_file(fid, rec)` 精确语义：
```
old = self.index.get(fid, {})
rec.setdefault("groups", [])
rec["groups"] = sorted(set(old.get("groups", [])) | set(rec.get("groups", [])))
self.index[fid] = rec        # 整体替换，不保留 rec 里没有的旧键（如 local_path）
self._save_index()
```

`_file_view(fid)`（对外视图，供 GUI 与事件使用）：
```
{"id", "name"(默认"?"), "size"(默认0), "src_uid"(默认""), "src_name"(默认""),
 "ts"(默认0), "groups"(默认[]), "has_local": bool(rec.has_local) and cache/<fid> 是文件}
```
`files_of_group(gid)` = 所有 `gid in rec.groups` 的 `_file_view`。
`has_file(fid)` = `index[fid].has_local` 为真 **且** `cache/<fid>` 是文件。
`index_record(fid)` = 原始记录或 `None`。

### 5.4 `cache/`

* 路径：`data_root / "cache"`，每次访问 `mkdir(parents=True, exist_ok=True)`。
* 缓存文件名 = 文件 ID（SHA-256 hex），无扩展名。
* 下载中临时文件：`cache/<fid>.part`。下载完成后：若 `cache/<fid>` 已存在则先 `unlink`，再 `rename` 成正式名。
* **失败时 `.part` 不删除**（源码无清理逻辑）——C++ 复刻需注意磁盘残留；「源码未定义」是否需要清理。

### 5.5 `downloads/`

* 路径：`data_root / "downloads" / f"{sanitize_name(gname, 30)}_{gid[:6]}"`，创建目录。
* 文件名：`sanitize_name(task["name"] or "file")`，默认 `maxlen=120`。
* `sanitize_name(s, maxlen)`：把 `\ / : * ? " < > | \r \n \t` 全部替换为 `_`，`strip()`；空串 → `"未命名"`；最后截断到 `maxlen`。
* 命名冲突处理（`_finish_download` 精确逻辑）：
  ```
  target = gdir / name
  if target 存在 and target.size != task.size:
      stem, suffix = splitext(name)
      n = 1
      while target 存在: target = gdir / f"{stem} ({n}){suffix}"; n += 1
  copy2(cache/<fid>, target)
  ```
  → 同名且**大小相同**时**直接覆盖**；大小不同才加 `(n)` 后缀。

### 5.6 `config.json` 中与本规格相关的字段

| key | 类型 | 默认 | 用途 |
| --- | --- | --- | --- |
| `uid` | string | 自动生成 | 设备 ID / `net_members` 键 |
| `name` | string | `hostname` | 显示名 |
| `tcp_port` | int | 45431 | TCP 端口（绑定失败会自动改写） |
| `udp_port` | int | 45455 | UDP 端口 |
| `groups` | array[{gid,gname}] | `[]` | 已加入的群 |
| `rename_count` / `rename_vote_need` | int | 0 / 5 | 改名（范围外） |

---

## 6. TCP 下载协议（精确到字节）

### 6.1 帧格式（`encode_frame` / `recv_json`）

```
+----------------+----------------------------+
| 4 字节大端长度 N | N 字节 UTF-8 JSON (ensure_ascii=false) |
+----------------+----------------------------+
```

* 长度 = 已序列化 JSON 字节数（不含 4 字节头）。
* 接收端若 `N > 64*1024*1024` → 抛 `ValueError("异常的数据帧长度")`。
* `recv_exact`：循环 `recv`，对端提前关闭 → `ConnectionError("连接被对端关闭")`。
* **文件正文不是 JSON 帧**，是紧跟头部应答之后的裸字节流（见下）。

### 6.2 请求方 → 服务端（客户端，`_fetch_from`）

步骤（严格按序）：

1. `ip` 为空或 `port == 0` → 直接返回失败（不建连接）。
2. 建 TCP socket，`connect((ip, port))`，**超时 8 秒**（用于连接 + 收头部）。
3. 发送 JSON 帧：`{"cmd": "get", "file_id": "<fid>"}`。**只有这两个字段**。
4. 接收一个 JSON 帧，记为 `hdr`：
   * `hdr["cmd"] != "data"` → 抛异常 `RuntimeError("对方应答：{hdr.get('err','未知错误')}")`；
   * 否则 `size = int(hdr["size"])`。
5. **无论成败**，`finally` 中把 socket 超时改为 **60 秒**（用于接收正文）。
6. 打开 `cache/<fid>.part`（`"wb"`），边收边写边算 SHA-256：
   * 循环 `recv(FILE_CHUNK = 131072)`；`recv` 返回空 → break；
   * 写入文件、`hash.update`、`done += len(blk)`；
   * `prog = done / size`（`size == 0` 时为 `1.0`）；
   * 若 `now - last_emit > 0.2` 或 `prog >= 1.0` → 推 `file_status{"id":fid,"status":"downloading","progress":min(prog,1.0),"note":"正在下载..."}`，并更新 `last_emit`。
7. `sock.close()`。
8. **完整性校验一（长度）**：`done != size` → 抛 `RuntimeError("数据不完整：期望 {size}，实际 {done}")`。
9. **完整性校验二（指纹）**：`sha256.hexdigest() != fid` → 抛 `RuntimeError("文件校验失败（指纹不一致）")`。
10. 若 `cache/<fid>` 已存在 → `unlink`；`tmp.rename(cache/<fid>)`；返回成功。

> 客户端**不发送任何结束/确认帧**；服务端靠对流结束（或发送完关闭）来终止。

### 6.3 服务端（`_handle_conn`）

* 每个连接一个独立线程（`tcp-conn`）。
* `conn.settimeout(30)`。
* 收一个 JSON 帧 `req`，按 `req["cmd"]` 分支：

| `cmd` | 请求字段 | 应答 |
| --- | --- | --- |
| `"get"` | `file_id`(string) | 若 `cache/<file_id>` 是文件 → 先发帧 `{"cmd":"data","file_id":fid,"name": index[fid].name 或 "", "size": cache.stat().st_size}`，随后以 `FILE_CHUNK = 131072` 的块 `sendall` 整个文件直到 EOF；否则发 `{"cmd":"err","err":"没有该文件"}` |
| `"secret_store"` | — | 范围外（转存） |
| `"secret"` | — | 范围外 |
| 其它 | — | `{"cmd":"err","err":"未知命令"}` |

* 任何异常被静默吞掉（`except Exception: pass`），`finally` 中关闭连接。
* 服务端**不校验请求方是否在内网、是否有该群权限**。

### 6.4 超时 / 重试 / 退避（准确值）

| 环节 | 值 |
| --- | --- |
| TCP connect + 头部接收超时 | 8 秒 |
| 正文接收超时 | 60 秒 |
| 服务端整体连接超时 | 30 秒 |
| 分块大小 | 131072 字节 |
| 进度上报节流 | 0.2 秒一次（或进度到 100%） |
| 公共询问等待 | `OFFER_WAIT = 8` 秒 |
| 回退重试上限 | `FALLBACK_RETRY = 2`（额外重试 2 次，共 3 次尝试） |
| **退避策略** | **没有指数退避、没有 sleep 间隔**：任务通过 `_download_queue.put(task)` 立刻重新入队；重试之间的等待完全来自下一次尝试里的 `OFFER_WAIT`（以及可能存在的直连超时）。源码未定义其它退避。 |
| 同一文件并发保护 | `self._downloading`（下载中集合）与 `self._active_downloads`（file_id → task） |

### 6.5 下载状态机（`_download_worker` / `_do_download`）

```
_download_worker 循环：
    task = queue.get(timeout=1.0)          # 空则继续
    try:
        _do_download(task, attempt = task.get("attempt", 0))
    except:
        推 file_status{status:"error", note:"下载失败，可手动重试"}
    finally:
        _downloading.discard(task.fid)
```

`_do_download(task, attempt)`：

1. `_downloading.add(fid)`；`_active_downloads[fid] = task`。
2. 推 `file_status{id, status:"downloading", progress:0.0, note:"正在下载（点对点）..."}`。
3. 推 `syschat(gid, "检测到「{gname}」群「{sender_name}」的新文件「{name}」，开始自动下载...")`。
4. **直连阶段**：若 `is_online(sender_uid)`（存在 peer、`now - peer.ts <= PRESENCE_TTL`、`peer.tcp_port != 0`）→ 用 `peer.ip`/`peer.tcp_port` 调 `_fetch_from`；成功 → `_finish_download(task)` 后 `return`。
5. **回退阶段**（发送人离线或直连失败）：
   ```
   req_id = _new_msg_id()
   _offers[req_id] = []
   _public_request(task, req_id)          # 广播 file_req
   deadline = now + OFFER_WAIT
   while running and now < deadline:
       offers = _offers[req_id]
       if offers:
           ip, port, uid = offers[0]
           ok = _fetch_from(ip, port, fid, task)   # 异常按失败处理
           if ok:
               _finish_download(task); _offers.pop(req_id); return
           offers.pop(0)                            # 第一个失效 → 试下一个
           continue
       sleep(0.3)
   ```
6. 超时后：
   * 若 `attempt < FALLBACK_RETRY` → 推 `syschat(gid, "文件「{name}」暂无人可提供，稍后自动重试（{attempt+1}/{FALLBACK_RETRY}）...")`；`task["attempt"] = attempt + 1`；重新入队。
   * 否则 → 推 `file_status{id, status:"error", note:"发送人离线且无缓存应答，可稍后重试"}`；`_active_downloads.pop(fid)`。

`_finish_download(task)`：

1. `cache = cache/<fid>`；`gdir = downloads_dir(gid, gname)`；按 §5.5 计算 `target` 并 `copy2`。
2. `_register_file(fid, {"name": name, "size": task.size, "ts": now,
   "src_uid": task.sender_uid, "src_name": task.sender_name,
   "groups": [task.gid], "has_local": true, "local_path": str(target)})`。
3. `_active_downloads.pop(fid)`。
4. 推 `notify("文件下载完成", "「{gname}」群「{sender_name}」的「{name}」下载完毕")`。
5. 推 `file_status{id, status:"done", path:str(target), note:"已下载"}`。
6. 推 `syschat(gid, "文件「{name}」下载完毕：{target}")`。
7. `broadcast_log("「{my_name}」下载了「{gname}」群「{sender_name}」的文件「{name}」")`。

> 注意：`_finish_download` 之后 `_downloading` 集合由 worker 的 `finally` 清理。

---

## 7. 内网状态机

### 7.1 状态定义

```
NO_NET        : self.net == None                      —— 未加入
APPLYING      : self.net == None and self.net_applying != None
IN_NET        : self.net != None and self.net.nid 非空
SUPPRESSED    : self._net_suppress_auto == True（用户手动退出过；仅内存标志，不落盘）
```

`_net_suppress_auto` **没有在 `__init__` 中初始化**，全部读取处用 `getattr(self, "_net_suppress_auto", False)`。

### 7.2 启动引导（`_net_loop` → `_net_bootstrap`，仅在 `start()` 后执行一次）

```
_net_loop:
    sleep(0.4)                       # 等 UDP 线程就绪
    if not running: return
    _net_bootstrap()                 # 只跑一次
    while running:
        _net_tick()
        sleep(2.0)
```

`_net_bootstrap()` 分支：

1. **已在网（`in_net()`）** → `_net_announce()`、`_net_sync()`、推 `net_changed` + `net_status("已在内网「{name}」（成员 {n} 人）")`。
2. **在申请中（`net_applying` 非空）** → 推 `net_status("正在等待内网成员投票…")`，`_net_last_req = 0`，`_net_join_req_broadcast()`，推 `net_changed`。
3. 否则 → `found = net_scan(NET_SCAN_WINDOW)`（阻塞 3 秒）。
   * 若 `_net_suppress_auto` 为真 → 只提示「已退出内网（附近发现 N 个内网）」，**不自动申请**。
   * 若 `found` 非空 → 取 `found[0]`（按 `net_nearby_list()` 排序：人数多优先，其次 `nid`）→ `net_apply_join(best.nid)`；抛异常 → `net_create()`。
   * 若 `found` 为空 → `net_create()`。

`net_scan(seconds=None)`（同步阻塞函数）：

```
seconds = NET_SCAN_WINDOW if seconds is None else float(seconds)
deadline = now + max(0.5, seconds)
推 net_status("正在扫描附近的内网（{seconds:.0f} 秒）…")
while running and now < deadline:
    广播 {"t":"net_scan","ts":now}
    sleep(1.0)
nets = [n for n in net_nearby.values() if now - n.ts <= NET_NEARBY_TTL]
推 net_nearby_changed
return nets
```

### 7.3 三种投票门槛（`net_vote_need`，精确算法）

```python
def net_vote_need(members):
    members = max(0, int(members))
    if members <= NET_LIMIT_NO_VOTE:          # <= 5
        return 0, f"内网 {members} 人（不超过 {NET_LIMIT_NO_VOTE} 人），免投票自动通过"
    if members <= NET_LIMIT_FULL:             # 6..7
        return members, f"内网 {members} 人（{NET_LIMIT_NO_VOTE+1}–{NET_LIMIT_FULL} 人），需全票通过（{members} 票）"
    need = members // 3 + 1                   # >= 8
    return need, f"内网 {members} 人（{NET_LIMIT_FULL+1} 人及以上），需超过 1/3 同意即 {need} 票"
```

* 参数 `members` 是 **`len(req["voters"])`**（申请记录里的投票人名单长度），不是 `net_members` 的长度。
* 返回值第二项是**必须逐字复刻**的中文说明串（GUI 与日志都直接展示）。

边界值（源码测试用例确认）：0→0，1→0，5→0，6→6，7→7，8→3，9→4，12→5，15→6，20→7。

`_req_progress(req)` 的派生统计：

```
voters = list(req.voters)
approvals = [m for m in voters if votes[m].approve is True]
rejects   = [m for m in voters if votes[m].approve is False]
abstains  = [m for m in voters if m in votes and votes[m].approve is None]
need, desc = net_vote_need(len(voters))
→ {"voters":len(voters), "need":need, "desc":desc,
   "approvals":len(approvals), "rejects":len(rejects), "abstains":len(abstains),
   "approvers":approvals, "rejecters":rejects}
```

### 7.4 申请流程（`net_apply_join`）

1. 已在网 → 抛 `RuntimeError("已经在内网里了")`。
2. `_net_suppress_auto = False`。
3. 从 `net_nearby[nid]` 取扫描结果；不存在 → 抛 `ValueError("附近没有这个内网（请重新扫描）")`。
4. **`voters = sorted(set(info.macs))`** —— 注意这是 `net_announce` 里 `sorted(net_members)[:64]` 的结果，因此**投票人名单 = 公告里前 64 个成员的设备 ID**。
5. `req_id = _new_msg_id()`，`created = now`。
6. 写 `net_applying = {req_id, nid, name: info.name, voters, created_ts, deadline: created + NET_VOTE_WINDOW}`。
7. 写 `net_reqs[req_id] = {req_id, nid, mac: self.uid, name: my_name, ip: get_local_ips()[0],
   tcp_port: my_tcp_port(), voters, votes: {}, state: "pending", note: "",
   created_ts, deadline}`。
8. `_save_net()`；`_net_last_req = 0`；`_net_join_req_broadcast()`。
9. 推 `net_status`（`need==0` 时 `"已提交申请：{desc}，等待成员确认…"`，否则 `"已提交申请：{desc}，等待成员投票（1 天内有效）"`）、`net_vote_changed`、`net_changed`。

`net_cancel_apply()`：撤销本地申请（`net_reqs.pop(req_id)`、`net_applying = None`、`_save_net()`、推 `net_status("已撤回入网申请")` + `net_vote_changed` + `net_changed`）。**不广播任何撤销报文**（源码未定义对端感知撤销的机制；对端只能靠超时）。

### 7.5 审批评估（`_net_eval_req`，投票结果判定）

按以下顺序**短路**执行（每一处都会立刻返回）：

```
1. 状态非 pending → 返回
2. 自己不在网 或 req.nid != self.net.nid → 返回
3. 若 req.mac 已在 net_members → state="approved", note="已是内网成员" → 返回（不广播）
4. prog = _req_progress(req); need = prog.need; voters = prog.voters
5. need == 0                      → _net_approve_req(req, "{desc}，自动通过")
6. prog.approvals >= need         → _net_approve_req(req, "同意 {approvals}/{voters} 票，已达门槛（{desc}）")
7. prog.rejects > 0 且 need >= voters
                                  → _net_reject_req(req, "有 {rejects} 名成员反对，无法全票通过")
8. now > req.deadline             → _net_reject_req(req, "投票已满 {hours} 小时仍只有 {approvals} 票（需 {need} 票），投票结束")
     其中 hours = max(1, int(NET_VOTE_WINDOW // 3600)) = 24
```

关键点：
* **第 7 条只在「全票通过」档（6–7 人，`need == voters`）生效**；`≥8` 人档出现反对票不会终止投票。
* 第 6 条用 `>=`，与「超过 1/3」的 `need = n//3+1` 组合后语义正确。
* **通过不广播结果**：`_net_approve_req` 只 `_save_net()` + `_net_sync()` + `broadcast_log` + 推事件。

`_net_approve_req(req, note)`：
```
req.state = "approved"; req.note = note; req.closed_ts = now
_net_add_member(req.mac, req.name, req.ip, req.tcp_port)     # founder 默认 False
_save_net(); _net_sync()
broadcast_log("「{name}」（物理地址 {MAC}）获准加入内网「{net.name}」：{note}")
推 net_changed + net_vote_changed
```

`_net_reject_req(req, note)`：
```
req.state = "rejected"; req.note = note; req.closed_ts = now
_save_net()
macs = sorted(set(req.voters) | {req.mac})
if macs and macs[0] == self.uid:          # 仅最小设备 ID 者广播
    广播 {"t":"net_join_result","req_id","nid","mac","approved":false,"reason":note,"ts":now}
推 log("[..] {name}（物理地址 {MAC}）入网申请未通过：{note}")
推 net_changed + net_vote_changed
```

### 7.6 加入成功（`_net_join_success(registry)`）

由 `_on_net_registry` 在「我未入网 + 登记表 members 里出现我的 uid」时调用：

```
a = net_applying or {}
nid = registry.nid or a.nid
self.net = {"nid": nid,
            "name": registry.net_name or a.name or f"内网{nid[:6]}",
            "founder_mac": registry.founder_mac or "",
            "created_ts": now, "joined_ts": now}
self.net_members = {}
for m in registry.members: _net_add_member(m.mac, m.name, m.ip, m.tcp_port, founder=m.founder)
_net_add_member(self.uid, my_name, get_local_ips()[0], my_tcp_port())
self.net_applying = None
_save_net(); _net_announce(); _net_sync()
broadcast_log("「{my_name}」（物理地址 {MAC}）加入了内网「{net.name}」")
推 net_changed + net_result(True, "已加入内网「{name}」（成员 {n} 人）")
```

### 7.7 成员登记（`_net_add_member`，返回是否有变更）

```
mac 为空 → return False
old = net_members.get(mac)
若不存在：
    net_members[mac] = {"mac", "name": name or "?", "ip": ip or "", "tcp_port": int(tcp_port or 0),
                        "joined_ts": now,
                        "founder": bool(founder or (net and net.founder_mac == mac))}
    return True
存在：
    对 (name, ip, tcp_port) 三个字段：新值非空且不同 → 覆盖（tcp_port 转 int，其余转 str），changed=True
    若 founder=True 且 old.founder 非真 → old.founder=True，changed=True
    return changed
```
**已存在成员不会更新 `joined_ts`**，`mac` 也不会被改写。

`net_view()` 中成员排序：`key = (not m.founder, float(m.joined_ts or 0))` —— **发起者第一，其余按加入时间升序**。

### 7.8 周期循环（`_net_tick`，每 2.0 秒一次）

```
now = now_ts()
若 in_net():
    若 now - _net_last_announce >= NET_ANNOUNCE_INTERVAL(15):
        _net_last_announce = now; _net_announce(); _net_sync()
    对 net_reqs 中所有 state == "pending" 的：_net_eval_req(req)
    若 now - _net_last_vote_replay >= 60:
        _net_last_vote_replay = now
        对 net_my_votes 中每个 (rid, mv)：
            若 net_reqs[rid] 存在且 state == "pending":
                广播 {"t":"net_join_vote","req_id":rid,"nid":req.nid,"voter":uid,
                      "voter_name":my_name,"approve":mv.approve,"ts":mv.ts}
        # 重播用“原始 ts”，接收端按 ts 大小去重
    _net_prune()
若 net_applying 非空:
    若 now - _net_last_req >= NET_REQ_REBROADCAST(10):
        _net_last_req = now; _net_join_req_broadcast(); 推 net_vote_changed
    若 now > applying.deadline:
        net_applying = None
        若 net_reqs[applying.req_id] 存在: state="rejected", note="投票超过 1 天，自动结束"
        _save_net(); 推 net_result(False, "入网申请超过 1 天仍未被通过，投票已结束") + net_changed
清理 net_nearby 中 now - ts > NET_NEARBY_TTL(40) 的条目（每条各推一次 net_nearby_changed）
```

`_net_prune()`：把 `state in ("approved","rejected")` 且 `created_ts < now - 7*86400` 的申请及其 `my_votes` 删除，有删除则 `_save_net()`。

### 7.9 建网 / 退出 / 改名

`net_create(name="")`：
1. 已在网 → 抛 `RuntimeError("已经在内网里了")`。
2. `_net_suppress_auto = False`。
3. `name = sanitize_name(name or f"{hostname} 的内网", 30)`。
4. `nid = secrets.token_hex(3).upper()`。
5. `net = {"nid", "name", "founder_mac": self.uid, "created_ts": now}`。
6. `net_members = {}`；`_net_add_member(self.uid, my_name, get_local_ips()[0], my_tcp_port(), founder=True)`。
7. `_save_net()`、`_net_announce()`、`_net_sync()`、`broadcast_log("「{my_name}」发起内网「{name}」（物理地址 {MAC}），等待同学加入")`。
8. 推 `net_status("附近没有发现内网，已自行建立内网「{name}」")` + `net_changed`。

`net_leave()`：
1. 不在网 → 直接返回。
2. 广播 `net_member_left{nid, mac: uid, name: my_name}`。
3. `broadcast_log("「{my_name}」退出了内网「{name}」")`（异常吞掉）。
4. 清空：`net=None`、`net_members={}`、`net_reqs={}`、`net_applying=None`、`net_my_votes={}`。
5. `_net_suppress_auto = True`。
6. `_save_net()`；推 `net_status("已退出内网：可在「内网」页手动申请加入其它内网，或自行建立内网")` + `net_changed`。
7. 起线程 `_net_rescan_after_leave`：`net_scan(NET_SCAN_WINDOW)` 后推 `net_changed`。

> **重入**：退出后本进程不会再自动申请（`_net_tick` 不含引导逻辑，`_net_bootstrap` 只在启动时跑一次）；用户须在 GUI 点「重新扫描」再申请，或直接自建。程序重启后 `_net_suppress_auto` 丢失，将再次自动扫描/申请/自建。
> **退出时不会从别人的 `net_reqs` 里撤销自己发起的申请**；也不会通知别处清理 `my_votes`。

`net_rename(name)`：
1. 不在网 → 抛 `RuntimeError("还没有加入内网")`。
2. `net.founder_mac != uid` → 抛 `PermissionError("只有内网的发起者可以重命名内网")`。
3. `name = sanitize_name(name, 30)`；改 `net.name`；`_save_net()`；`_net_announce()`；`_net_sync()`；`broadcast_log("内网「{old}」更名为「{name}」")`；推 `net_changed`。
   注意：**改名不产生独立报文**，靠 `net_announce`/`net_registry` 的 `net_name` 传播。

### 7.10 投票有效期 / 改票窗口 / 重播（汇总）

| 机制 | 准确实现 |
| --- | --- |
| 有效期 | 申请记录 `deadline = created_ts + NET_VOTE_WINDOW`（86400 秒）。成员侧在 `_net_eval_req` 第 8 步按 `now > deadline` 判超时拒绝；申请人侧在 `_net_tick` 按 `now > applying.deadline` 判超时失败 |
| 超时文案 | 成员侧：`"投票已满 24 小时仍只有 {approvals} 票（需 {need} 票），投票结束"`；申请人侧：`"入网申请超过 1 天仍未被通过，投票已结束"` |
| 改票窗口 | `net_cast_vote` 中：若已投过且 `now - prev.ts > NET_VOTE_CHANGE(300)` → 抛 `RuntimeError("投票已超过 5 分钟，不能再改票")`（`int(NET_VOTE_CHANGE // 60)` = 5） |
| 弃权 | `approve` 传 `None`（Python）；线上字段为 JSON `null`；不参与 approvals/rejects 计数 |
| 票去重 | 接收端按 `voter`（设备 ID）一个键，**后到的 `ts` 更大才生效** |
| 本机投票去重 | `net_cast_vote` 直接覆盖 `net_my_votes[req_id]`，没有「同一提案只能投一次」的硬限制，只有改票窗口限制 |
| 重播 | 每 60 秒重播所有仍 pending 的自投票（用原始 ts）；申请人每 10 秒重播 `net_join_req`；每 15 秒广播 `net_announce` + `net_registry` |
| 历史保留 | 已结束申请保留 7 天（`_net_prune`），并在结束后 1 小时内仍随 `net_registry` 广播 |
| 撤销 | 只有申请人本地 `net_cancel_apply`，无广播 |

---

## 8. 推给 GUI 的事件（kind 与字段）

`_emit(kind, *payload)` 等价于 `events.put((kind,) + payload)`；GUI `_poll_events` 每 100 ms 排空队列一次。**C++ 侧只要复刻下表的 kind 与载荷，即可对接现有 GUI 契约。**

| kind | 载荷（按位置） | 类型 | 触发点 |
| --- | --- | --- | --- |
| `syslog` | `text` | string | `start()` |
| `log` | `line` | string（已含时间戳前缀的整行） | 见各处理函数 |
| `syschat` | `gid`, `text` | string, string | 下载开始/重试/完成、发送文件失败 |
| `chat` | `dict` | obj | 见下 |
| `dm` | `dict` | obj | 范围外 |
| `dm_sent` | `dict` | obj | 范围外 |
| `peers_changed` | 无 | — | `_on_presence`、`_sweep_loop` |
| `groups_changed` | 无 | — | 建群/入群/退群 |
| `name_changed` | `new_name` | string | 改名成功 |
| `file_new` | `dict` | obj | `send_file_to_group`、`_on_file_pub` |
| `file_status` | `dict` | obj | 下载全过程 |
| `notify` | `title`, `body` | string, string | `_finish_download` |
| `net_changed` | 无 | — | 内网任何结构变化 |
| `net_vote_changed` | 无 | — | 投票相关变化 |
| `net_status` | `text` | string（GUI 截断到 80 字） | 扫描/申请/建网/退网提示 |
| `net_nearby_changed` | 无 | — | 附近内网列表变化 |
| `net_result` | `ok`(bool), `text`(string) | — | 申请成功/失败 |
| `rename_request` / `rename_vote_progress` / `rename_vote_done` | — | — | 范围外 |
| `update` | `dict` | obj | 范围外 |

**`file_new` 载荷**：
```
{
  "file": {                      # 即 _file_view()
      "id": string, "name": string, "size": int,
      "src_uid": string, "src_name": string, "ts": float,
      "groups": [string], "has_local": bool
  },
  "gid": string,
  "from_self": bool
}
```

**`file_status` 载荷**（`status` 取值决定其它键是否存在）：

| `status` | 附加键 |
| --- | --- |
| `"downloading"` | `progress`: float 0.0–1.0，`note`: string |
| `"done"` | `path`: string（下载目录绝对路径），`note`: string（`"已下载"`） |
| `"error"` | `note`: string（`"下载失败，可手动重试"` / `"发送人离线且无缓存应答，可稍后重试"`） |

所有 `file_status` 都带 `"id": string`。

**GUI 侧消费要点**（复刻 GUI 时保证字段兼容）：
* `_on_ev_file_new` 用 `info["gid"]`、`info["file"]["id"/"name"/"size"/"src_name"/"has_local"]`，初始状态 `已存在`（`has_local` 真）/ `等待下载`。
* `_on_ev_file_status` 用 `st["id"]`、`st["status"]`、`st.get("progress")`、`st.get("path")`、`st.get("note")`。
* 手动「重试下载」直接构造任务压入 `_download_queue`：
  `{"fid": rec.id, "name": rec.name, "size": rec.size, "gid": gid, "gname": g.gname,
    "sender_uid": index_record(fid).src_uid, "sender_name": rec.sender}`（**不带 `attempt`**）。

---

## 9. 关键算法伪代码（可直接照抄实现）

### 9.1 文件发送

```
FUNCTION send_file_to_group(path, gid):
    require_net()
    ASSERT path 是文件
    g = groups[gid]; ASSERT g 存在
    fsize = size(path)
    emit syschat(gid, "正在计算文件指纹（{human_size(fsize)}）...")
    fid  = sha256_hex(path, chunk=131072)          # 小写 hex，64 字符
    cache = cache_dir()/fid
    IF NOT cache.exists(): copy2(path, cache)
    register_file(fid, {name: path.name, size: fsize, ts: now,
                        src_uid: uid, src_name: my_name,
                        groups: [gid], has_local: true})
    broadcast({t:"file_pub", gid, gname: g.gname,
               file:{id:fid, name:path.name, size:fsize},
               ts:now, sender_uid:uid, sender_name:my_name})
    broadcast_log("「{my_name}」在群「{g.gname}」发送了文件「{path.name}」")
    emit file_new({file: file_view(fid), gid, from_self: true})
```

### 9.2 收到公告 → 自动下载排队

```
FUNCTION on_file_pub(msg):
    IF msg.gid not in groups: RETURN
    IF NOT member_ok(msg.uid): RETURN
    fid = msg.file.id; IF 空: RETURN
    gname       = msg.gname 或 groups[msg.gid].gname
    sender      = msg.sender_name 或 msg.name 或 "?"
    sender_uid  = msg.sender_uid 或 msg.uid
    emit log("[{fmt_time(msg.ts)}] {sender}: 在群「{gname}」发送了文件「{msg.file.name}」")
    emit file_new({file:{id:fid, name, size, src_uid:sender_uid, src_name:sender,
                         ts:msg.ts, groups:[msg.gid], has_local:has_file(fid)},
                   gid:msg.gid, from_self:false})
    IF has_file(fid): RETURN
    IF fid in _downloading OR fid in _active_downloads: RETURN
    _download_queue.put({fid, name, size, gid, gname, sender_uid, sender_name})
```

### 9.3 下载主流程（含回退）

```
FUNCTION do_download(task, attempt):
    fid = task.fid
    _downloading.add(fid); _active_downloads[fid] = task
    emit file_status({id:fid, status:"downloading", progress:0.0, note:"正在下载（点对点）..."})
    emit syschat(task.gid, "检测到「{gname}」群「{sender_name}」的新文件「{name}」，开始自动下载...")

    # 1) 直连
    IF is_online(task.sender_uid):
        p = peers[task.sender_uid]
        ok = try fetch_from(p.ip, p.tcp_port, fid, task)
        IF ok: finish_download(task); RETURN

    # 2) 公共询问
    req_id = new_msg_id()
    _offers[req_id] = []
    broadcast({t:"file_req", req_id, gid, gname, sender_uid, sender_name,
               file:{id:fid, name, size}, ts:now})
    emit log("[..] {my_name}: 公共询问文件「{name}」（发送人 {sender_name}，群「{gname}」）")
    deadline = now + OFFER_WAIT
    WHILE running AND now < deadline:
        IF _offers[req_id] 非空:
            (ip, port, uid) = _offers[req_id][0]
            ok = try fetch_from(ip, port, fid, task)
            IF ok:
                finish_download(task); _offers.remove(req_id); RETURN
            _offers[req_id].pop_front()
            CONTINUE
        sleep(0.3)

    # 3) 重试
    IF attempt < FALLBACK_RETRY:
        emit syschat(task.gid, "文件「{name}」暂无人可提供，稍后自动重试（{attempt+1}/{FALLBACK_RETRY}）...")
        task.attempt = attempt + 1
        _download_queue.put(task)
    ELSE:
        emit file_status({id:fid, status:"error", note:"发送人离线且无缓存应答，可稍后重试"})
        _active_downloads.remove(fid)
```

### 9.4 TCP 取文件

```
FUNCTION fetch_from(ip, port, fid, task) -> bool:
    IF ip 为空 OR port == 0: RETURN false
    sock = connect(ip, port) with timeout 8s
    TRY:
        send_json(sock, {cmd:"get", file_id:fid})
        hdr = recv_json(sock)
        IF hdr.cmd != "data": RAISE "对方应答：" + (hdr.err or "未知错误")
        size = int(hdr.size or 0)
    FINALLY:
        sock.set_timeout(60)
    tmp = cache_dir()/ (fid + ".part")
    h = sha256_new(); done = 0; last_emit = 0
    WITH open(tmp, "wb"):
        WHILE running:
            blk = sock.recv(131072)
            IF blk 为空: BREAK
            write(blk); h.update(blk); done += len(blk)
            prog = (done / size) IF size != 0 ELSE 1.0
            IF now - last_emit > 0.2 OR prog >= 1.0:
                emit file_status({id:fid, status:"downloading",
                                  progress:min(prog,1.0), note:"正在下载..."})
                last_emit = now
    sock.close()
    IF done != size: RAISE "数据不完整：期望 {size}，实际 {done}"
    IF h.hexdigest() != fid: RAISE "文件校验失败（指纹不一致）"
    IF cache_dir()/fid 存在: unlink 它
    rename(tmp, cache_dir()/fid)
    RETURN true
```

### 9.5 TCP 服务端

```
FUNCTION handle_conn(conn, addr):
    conn.set_timeout(30)
    TRY:
        req = recv_json(conn)
        IF req.cmd == "get":
            fid = str(req.file_id or "")
            cache = cache_dir()/fid
            IF fid 非空 AND cache 是文件:
                send_json(conn, {cmd:"data", file_id:fid,
                                 name: index[fid].name or "",
                                 size: size(cache)})
                WITH open(cache, "rb"):
                    WHILE true:
                        blk = read(131072); IF 空: BREAK
                        conn.sendall(blk)
            ELSE:
                send_json(conn, {cmd:"err", err:"没有该文件"})
        ELSE IF req.cmd == "secret_store": ...     # 范围外
        ELSE IF req.cmd == "secret": ...            # 范围外
        ELSE:
            send_json(conn, {cmd:"err", err:"未知命令"})
    CATCH: 静默
    FINALLY: conn.close()
```

### 9.6 公共询问应答

```
FUNCTION on_file_req(msg, addr):
    fid = msg.file.id; req_id = msg.req_id
    IF fid 为空 OR req_id 为空: RETURN
    emit log("[{fmt_time(msg.ts)}] {msg.name}: 公共询问文件「{msg.file.name}」（发送人 {msg.sender_name}，群「{msg.gname}」）")
    IF has_file(fid):
        broadcast({t:"file_have", req_id, file_id:fid,
                   ip: get_local_ips()[0], port: my_tcp_port(), ts: now})

FUNCTION on_file_have(msg):
    IF msg.req_id in _offers:
        _offers[msg.req_id].append((msg.ip, int(msg.port or 0), msg.uid))
```

### 9.7 内网引导（启动）

```
FUNCTION net_bootstrap():
    IF in_net():
        net_announce(); net_sync(); emit net_changed
        emit net_status("已在内网「{name}」（成员 {n} 人）"); RETURN
    IF net_applying:
        emit net_status("正在等待内网成员投票…")
        _net_last_req = 0; net_join_req_broadcast(); emit net_changed; RETURN
    found = net_scan(NET_SCAN_WINDOW)          # 阻塞 3 秒，每 1 秒广播 net_scan
    IF _net_suppress_auto:
        emit net_status("已退出内网（附近发现 {len} 个内网）：可在「内网」页手动加入或自行建立")
        emit net_changed; RETURN
    IF found 非空:
        best = found[0]                        # 人数多者优先，其次 nid 字典序
        emit net_status("发现内网「{best.name}」（{best.member_count} 人），正在申请加入…")
        TRY net_apply_join(best.nid) CATCH: net_create()
    ELSE:
        net_create()
```

### 9.8 收包时更新附近内网

```
FUNCTION on_net_scan(msg, addr):
    IF NOT in_net(): RETURN
    IF now - _net_reply_ts < 0.5: RETURN
    _net_reply_ts = now
    net_announce()                             # 必须广播应答

FUNCTION on_net_announce(msg, addr):
    IF msg.nid 为空: RETURN
    IF in_net() AND msg.nid == net.nid: RETURN
    net_nearby[msg.nid] = {nid: msg.nid,
                           name: msg.net_name or "内网{nid[:6]}",
                           founder_mac: msg.founder_mac or "",
                           member_count: int(msg.member_count or 0),
                           macs: list(msg.macs or []),
                           ip: addr.ip if addr else "",
                           ts: now}
    emit net_nearby_changed
```

---

## 10. 移植风险点（按严重度）

1. **「通过」没有专门报文，只有 `net_registry` 里的成员表**（§4.5/§7.6）。申请人只有在 `net_registry.members` 里看到自己的 `uid` 才算加入成功；且 `net_join_result` 的 `approved` 只发 `false`。C++ 若自作聪明加一个「approved 单播确认」，会造成与 Python 端互不兼容。
2. **投票人名单 = `net_announce.macs` 的前 64 个排序项，而不是实际成员集合**（§4.2/§7.4）。`voters` 决定门槛（`net_vote_need(len(voters))`）：成员超过 64 人、或申请时有人掉线未应答，都会让门槛算错。`net_join_req` 收到后会把 `self.uid` 并进 `voters` 再传播，`net_registry` 又会做并集合并 —— 这个「名单会增长」的传播链必须逐字复刻。
3. **票的时间戳语义（`ts` 单调获胜）与 60 秒重播**（§4.4/§7.8）。接收端只认 `ts` 更大的票，重播必须带**原始 ts**；同时改票窗口 `NET_VOTE_CHANGE` 是按**本机 `net_my_votes[req].ts`** 判断的。C++ 若在重播时刷新 `ts`，会把「投出新票」与「重播旧票」混淆，导致改票结果被旧票覆盖或永远无法改票。

其它需要留意的次级风险：

* 报文类型键是 `t`，不是 `type`；`uid`/`name`/`msg_id` 由发送层自动 `setdefault` 补齐。
* `_member_ok` 在「自己未入网」时返回 **true**（不限制），并且 `_on_file_req`/`_on_file_have` 根本不做成员检查。
* `net_scan` 是**阻塞函数**，`_net_loop` 与 GUI 线程都会调用它；移植时要明确线程模型。
* `net_cancel_apply` 无网络报文；`net_leave` 不从他人的 `net_reqs` 中撤销自己的申请。
* `.part` 文件在失败后不清理；`downloads` 同名同大小文件会被直接覆盖。
* `net.json` 的 `applying` 恢复被嵌在 `net.nid` 非空分支里，实际不会生效（§5.2 备注）。
* `_net_suppress_auto` 不落盘：重启即恢复自动组网。
* `NET_NO_MEMBER` 常量定义了但从未使用。

---

## 11. 「源码未定义」清单（C++ 需自行决策并记录）

| 项 | 说明 |
| --- | --- |
| 版本协商 | TCP/UDP 报文除 `presence.ver`（仅记录、从不比较）外，没有任何协议版本字段 |
| 下载退避 | 除 `OFFER_WAIT` 每次 8 秒外，无指数退避、无抖动、无上限递增 |
| `.part` 清理 | 失败/中断的 `.part` 是否清理、何时清理，源码未定义 |
| 取消下载 | 无「中止下载」协议；`_running=false` 是唯一打断方式 |
| `net_registry` 载荷大小上限 | 除 `reqs[-30:]` 与 `macs[:64]` 外，无整体报文大小限制（UDP 可能超 MTU 被分片/丢弃） |
| 撤销申请的对端感知 | 无报文 |
| 退出内网后的历史清理 | 本地 `net_nearby` 仍保留；他人的 `net_reqs` 不清 |
| `state` 的其它取值 | 仅 `pending` / `approved` / `rejected` 三种被使用 |
| 同一 `uid` 多实例 | 同一台机器多开时 `uid` 相同，`uid == self.uid` 的报文会被自己丢弃 |
| 文件权限/内网校验 | TCP 服务端不校验请求者身份与群归属 |
