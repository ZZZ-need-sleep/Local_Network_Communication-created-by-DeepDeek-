# 内部网 局域网群组 v2.0.0 设计契约（新功能 + 双更新程序 + C++ 版）

> 本文件是**唯一权威契约**。Python 版（`neiwang.py`）与 C++ 版（`cpp/`）的新功能实现必须逐字遵循本文件的
> 报文名、字段名、文件格式、算法与日志文案。任何偏离都必须先改本文件。

- 版本号：`update.wenyi` 第一行 = `2.0.0`（两版共用同一个清单文件）。
- 兼容性要求：**与 v1.1 的旧客户端（Python）在同一局域网内必须继续互通**。
  → 所有新增 UDP 报文使用新的 `t` 值，旧客户端在 `_dispatch_udp` 里落到 `else`（无分支）被静默忽略；
  → 所有新增 TCP 命令使用新的 `cmd` 值，旧服务端回 `{"cmd":"err","err":"未知命令"}`，调用方必须容错；
  → 不得修改任何 v1.1 已有报文的字段语义。

---

## 0. 术语

| 术语 | 含义 |
| --- | --- |
| 内网 / 局域网组织 | v1.1 的「内网」准入层，`net.json`，成员以物理地址（MAC）登记 |
| 活动历史 | 新功能 A：持久化的局域网全局日记（公共日志）操作数据，可在设备间补齐 |
| 最后一次数据获取时间 | `last_get_ts`，本机最后一次从局域网取得/合并活动数据的时间 |
| 连续运行起始 | `run_since`，本进程本次连续运行的起始时间（**仅内存，不持久化**） |
| 代理 / 数据代理 | 新功能 B：群内无人在线时，把文件或信息临时寄存到局域网其他在线设备上 |
| 更新程序1 / 更新程序2 | 双更新程序方案，见第 4 节 |

---

## 1. 新功能 A：局域网活动历史与补齐

### 1.1 数据文件 `data/active_log.json`

```json
{
  "net_id": "ABC123",
  "my_mac": "AABBCCDDEEFF",
  "last_get_ts": 1760100000.0,
  "first_ts": 1760000000.0,
  "seq": 42,
  "events": [
    {"eid": "AABBCCDDEEFF-1", "seq": 1, "ts": 1760000000.0,
     "uid": "AABBCCDDEEFF", "name": "设备一", "kind": "log",
     "text": "「设备一」加入了内网「教室」"}
  ]
}
```

- `eid`：事件唯一 ID = `uid + "-" + seq`，用于跨设备去重合并。
- `seq`：制造该事件的设备自己的单调序号，从 1 开始。
- `kind` 取值：`log`（公共日志行）、`chat`（群聊）、`proxy`（代理操作）、`sys`（系统/上线提醒）。
- `text`：已格式化好的显示文本（**与 UI 日志行完全一致**），便于跨设备直接展示。
- 上限 `ACT_MAX_EVENTS = 5000`：超出时丢弃最旧的（`first_ts` 随之更新）。
- 文件不存在 / 无法解析 = 「本机没有任何局域网活动数据」。

### 1.2 记录活动事件

新增方法 `record_activity(kind, text, ts=None) -> str`：

1. `seq = self.act_seq + 1`；`eid = f"{uid}-{seq}"`；
2. 追加到 `self.act_events`；`self.act_seq = seq`；`first_ts` 若为空则设为 ts；
3. 落盘 `active_log.json`（节流：最多每 0.5s 一次，退出时强制落盘）。

调用点（两版一致）：

- `broadcast_log()` 里，广播前调用 `record_activity("log", f"{self.my_name}: {text}")`；
- `_on_log()` 收到别人的日志时调用 `record_activity("log", f"{name}: {text}", ts=msg.ts)`；
- `send_chat()` / `_on_chat()` 记 `kind="chat"`；
- 代理模块的所有可见操作记 `kind="proxy"`（见第 2 节日志文案）；
- 启动、补齐、上线提醒记 `kind="sys"`。

### 1.3 UDP 报文

新增常量：
```
ACT_HELLO_INTERVAL = 60.0    # 启动问询与周期问询间隔（秒）
ACT_HAVE_THROTTLE  = 0.5     # 应答节流（秒）
ACT_PULL_WAIT      = 6.0     # 等待应答/取数的总窗口（秒）
```

#### (1) `t = "act_hello"` — 本机活动数据问询（启动时 + 每 60s）

由**有活动数据的设备**（或任何设备）广播：

```json
{"t":"act_hello","uid":..,"name":..,"msg_id":..,
 "mac":"AABBCCDDEEFF",            // 物理地址
 "ip":"192.168.1.5",              // 本机局域网 IP
 "port":45431,                    // TCP 端口
 "last_get_ts":1760100000.0,      // 最后一次数据获取时间
 "first_ts":1760000000.0,
 "count":128,                     // 本机活动事件条数
 "run_since":1759990000.0,        // 本进程连续运行起始时间
 "net_id":"ABC123",               // 可为空（未入网时）
 "ts":1760100010.0}
```

- 未加入内网也允许广播（这样「空设备」能发现局域网里是否有活动）。但应答方只在 `net_id` 相同或对方未入网时按需应答；**应答方必须是内网成员**（对方未入网则不应答，避免向陌生人泄露）。
  简化实现：`in_net()` 为假时不应答；发送方 `net_id` 与自己不同也不应答。

#### (2) `t = "act_have"` — 相关数据校验应答

内网成员收到 `act_hello` 后（节流 `ACT_HAVE_THROTTLE`）立即广播应答：

```json
{"t":"act_have","uid":..,"name":..,"msg_id":..,
 "mac":"...","ip":"...","port":45431,
 "last_get_ts":..,"first_ts":..,"count":..,"run_since":..,
 "max_ts":1760100000.0,     // 本机活动事件里最大的 ts
 "sig":"<前16位十六进制>",   // 校验指纹（**精确公式**）：
                            //   sig = sha256( mac + "|" + str(count) + "|" +
                            //                ("%.3f" % last_get_ts) + "|" + ("%.3f" % max_ts) )[:16]
                            //   两个浮点一律保留 3 位小数（C++ 用 snprintf("%.3f")，Python 用 "%.3f" %）
 "net_id":"ABC123","ts":..}
```

请求方收到应答后写入日志（逐条）：

```
[时间] 收到「设备二」（物理地址 11-22-33-44-55-66）的局域网活动校验数据：连续运行自 10-09 21:00:00，最后数据 10-10 08:00:00，共 128 条
```

#### (3) 补齐决策（请求方 = 刚启动的设备）

设本机 `my_last = last_get_ts`（没有数据时用 `0`）。

- **若本机没有任何数据**（`events` 为空）：不需要补齐；按第 3 节走「空设备」提示流程。
- **若本机有数据**：在 `ACT_PULL_WAIT` 窗口内收集 `act_have` 应答。
  - **合格应答**：`run_since <= my_last + 1`（对方从我的最后数据时间之前就一直运行 → 它能覆盖我的空档）。
  - **选择**：合格应答中**最先到达的**（最快响应）。
  - 若没有合格应答，则退化为：`count` 更多者优先，其次 `max_ts` 更大者优先；都没有则放弃并记日志。
- 选中后建立 TCP 连接补齐；日志逐条：

```
[时间] 已选定响应最快的「设备二」（物理地址 11-22-33-44-55-66）构建连接补齐本机活动数据
[时间] 已从「设备二」补齐 10-09 22:00:00 至 10-10 08:00:00 的局域网活动信息（共 128 条）
```

- 若 6 秒内没有任何应答：日志 `[时间] 局域网内暂无可用于补齐活动数据的设备（本机最后数据时间 10-09 22:00:00）`。

### 1.4 TCP 命令（复用主 TCP 端口）

请求（UTF-8 JSON + 4 字节大端长度帧，v1.1 已有格式）：

```json
{"cmd":"act_get","since":1760100000.0,"mac":"AABBCCDDEEFF","name":"设备一"}
```

应答：

```json
{"cmd":"act_data","ok":true,
 "mac":"112233445566","name":"设备二",
 "first_ts":..,"last_get_ts":..,"count":..,
 "events":[ {"eid":..,"seq":..,"ts":..,"uid":..,"name":..,"kind":..,"text":..} ]}
```

- `events` 只包含 `ts > since` 的事件，且**不超过 2000 条**（从最早开始截断，保证空档连续）。
- 失败应答：`{"cmd":"err","err":"..."}`（调用方容错）。

### 1.5 合并规则

`merge_activity(events)`：

1. 以 `eid` 去重（本地已存在的跳过）；
2. 追加后按 `(ts, eid)` 升序排序；
3. `seq` 取本地已有的最大 seq（**不**采用对方的 seq，避免序号冲突；`eid` 含 uid 已保证全局唯一）；
4. `first_ts = min(...)`；`last_get_ts = now_ts()`；
5. 截断到 `ACT_MAX_EVENTS`；
6. 落盘；推给 GUI 事件 `("act_log", [新增的显示行...])` 以刷新日志页。

### 1.6 对外提供数据（应答方）

收到 `act_get` 时：把本机 `events` 中 `ts > since` 的前 2000 条返回。**这是「整晚在线的设备」的职责。**
并在日志里记：`[时间] 已向「设备一」提供 10-09 22:00:00 至 10-10 08:00:00 的局域网活动信息（共 128 条）`。

### 1.7 GUI

- 「日志」页在启动后必须能直接看到 1.3 节的全部广播/应答/补齐日志行。
- 日志页新增一行状态条：`局域网活动：最后一次数据获取时间 10-10 08:00:00 · 活动记录 128 条`。

---

## 2. 新功能 B：设备数据传输代理

### 2.1 目录与文件

- 代理目录：`data/proxy/`（**界面上不可见、不可打开**）。
- 文件代理落盘名：`<proxy_id>.proxy`（`proxy_id` 为 32 位十六进制随机串）。
  「后缀自动修改」= 代理文件一律以 `.proxy` 结尾，且原文件名只记录在元数据里。
- 元数据：`data/proxy.json`：

```json
{
  "<proxy_id>": {
    "proxy_id": "...", "kind": "file" | "msg",
    "gid": "FF856B", "gname": "qqq",
    "file_id": "<sha256>", "name": "作业.txt", "size": 1234,
    "text": "",                      // kind=msg 时的信息正文
    "sender_uid": "...", "sender_name": "...",
    "from_mac": "...",               // 我从谁那里收到的（我自己创建时 = 本机 mac）
    "created_ts": 0.0, "ts": 0.0,
    "targets": ["mac", "..."],       // 该群的其他成员（需要送达的人）
    "delivered_to": ["mac", "..."],  // 已确认送达过的人（幂等）
    "holders": ["mac", "..."],       // 当前仍持有该代理的设备（含本机）
    "consumed": false, "holder": ""  // 被接受后 holder = 接受者 mac（无人保留时 "")
  }
}
```

- 只有**内网成员**参与代理；代理记录**不进入 UI 列表**，只在日志里体现。

### 2.2 何时启动代理

`send_file_to_group(path, gid)` 与 `send_group_message(gid, text)`（新增：群内**无人在线**时的普通信息代理）在广播之后检查：

```
others = [m for m in group_members(gid) if not m.self and is_online(m.uid)]
if not others:  start_proxy(kind, gid, ...)
```

**注意**：v1.1 的 `is_online()` 要求 `tcp_port` 有效。v1.1 的 `group_members()` 用 `PRESENCE_TTL` 判定，
与 `is_online()` 略有差异；代理的「无人在线」判定统一使用 `is_online()`。

### 2.3 UDP 报文

新增常量：
```
PROXY_REQ_WAIT        = 10.0     # 等待在线应答（秒）
PROXY_GROUP_WAIT      = 300.0    # 群成员在这段时间内都没上线就向其他局域网成员扩散
PROXY_PUSH_TIMEOUT    = 15.0
```

#### (1) `t = "proxy_req"` — 在线请求

```json
{"t":"proxy_req","uid":..,"name":..,"msg_id":..,
 "req_id":"<uid-n-xxxxxx>",
 "gid":"FF856B","gname":"qqq",
 "kind":"file","file":{"id":"<sha256>","name":"作业.txt","size":1234},
 "text":"",                        // kind=msg 时
 "targets":["mac","..."],          // 该群其他成员（用于扩散判断）
 "sender_uid":"...","sender_name":"...",
 "net_id":"ABC123","ts":..}
```

日志（发送方本机）：`[时间] 「设备一」发起在线请求：群「qqq」当前没有其他成员在线，向局域网内所有在线成员请求文件代理`

#### (2) `t = "proxy_ack"` — 在线应答

收到 `proxy_req` 的内网成员（`is_online` 且 `uid != 发送方`）立即广播应答：

```json
{"t":"proxy_ack","uid":..,"name":..,"msg_id":..,
 "req_id":"...","mac":"...","ip":"...","port":45431,"ts":..}
```

发送方日志：`[时间] 「设备二」（物理地址 11-22-33-44-55-66）回复在线`

等待 `PROXY_REQ_WAIT` 秒后，对**每一个**回复在线的成员：
1. 通过 TCP `proxy_store` 寄存代理；
2. 成功则日志 `[时间] 已向「设备二」发送文件代理「作业.txt」（临时寄存，用户不可查看）`；
   `kind=msg` 时文案为 `已向「设备二」发送信息代理`。

若期间无任何应答：日志 `[时间] 局域网内没有其他在线成员可以代存，代理未建立（文件仍保留在本机）`。

#### (3) `t = "proxy_online"` — 上线提醒

任何设备在启动后广播一次，并在收到 `presence` 时由接收方本地产生提醒（不额外广播）；
同时该设备周期性（复用 `ACT_HELLO_INTERVAL`）随 `act_hello` 隐含上报。为清晰起见使用独立报文：

```json
{"t":"proxy_online","uid":..,"name":..,"msg_id":..,
 "mac":"...","groups":["FF856B"],"ts":..}
```

发送时机：`start()` 之后 1 秒广播一次；此后每 `PRESENCE_INTERVAL * 4` 秒广播一次。

**收到 `proxy_online` 或 `presence`（新出现/重新上线的 uid）时**，先写上线提醒日志：
`[时间] 上线提醒：「设备二」（物理地址 11-22-33-44-55-66）已上线`

然后对本地每一个**未 `consumed`** 的代理记录执行 `proxy_on_peer_online(proxy, mac, uid, name, groups)`：

1. `mac == proxy.sender_uid` → **回传原作者**：`proxy_push` 给该设备；确认后广播
   `proxy_consumed{proxy_id, holder:""}` 并删除本地代理。
   日志：`[时间] 原作者「设备一」已上线，已将文件代理「作业.txt」回传并删除本地代理`。
2. `mac in proxy.targets`（群内成员）→ **送达目标成员**：`proxy_push`；确认后广播
   `proxy_consumed{proxy_id, holder: mac}` 并删除本地代理（接受者成为新的分发源）。
   日志：`[时间] 群「qqq」成员「设备三」已上线，已将文件代理「作业.txt」送达并删除本地代理`。
3. 否则（局域网内其他成员）→ 若 `now - proxy.created_ts >= PROXY_GROUP_WAIT`
   且**该代理创建后没有任何 target 上线过**（`no_target_online`），则**扩散副本**：
   `proxy_push` 给该成员（不删除本地、不 consumed）。
   日志：`[时间] 群「qqq」成员一直未上线，已将文件代理「作业.txt」发送给本局域网成员「设备四」一份`。

### 2.4 TCP 命令

#### (1) `cmd = "proxy_store"` — 寄存代理

```json
{"cmd":"proxy_store","proxy_id":"...","kind":"file","gid":"..","gname":"..",
 "file_id":"<sha256>","name":"作业.txt","size":1234,"text":"",
 "sender_uid":"..","sender_name":"..","targets":[".."],"ts":..}
```

- 接收方：把元数据写入 `proxy.json`；`kind=file` 时**立即**回到寄存方用 `{"cmd":"get","file_id":..}`
  把文件内容拉下来，写入 `data/proxy/<proxy_id>.proxy`。
- 应答：`{"cmd":"proxy_stored","proxy_id":"...","ok":true}`；失败 `ok:false` + `err`。
- 接收方日志：`[时间] 已代「设备一」寄存文件代理「作业.txt」（用户不可查看，后缀已改为 .proxy）`

#### (2) `cmd = "get"` 扩展

接受可选字段 `proxy_id`：当 `file_id` 在普通 `cache/` 中不存在但 `proxy.json` 里有该
`file_id` 且 `proxy_id` 匹配且本地文件 `data/proxy/<proxy_id>.proxy` 存在时，按同样格式返回数据。

#### (3) `cmd = "proxy_push"` — 把代理推给对方

```json
{"cmd":"proxy_push","proxy_id":"...","kind":"file","gid":"..","gname":"..",
 "file_id":"..","name":"..","size":0,"text":"","sender_uid":"..","sender_name":"..",
 "targets":[".."],"ts":..}
```

- 接收方：`kind=file` → 立即用 `{"cmd":"get","file_id":..,"proxy_id":..,"from":"push"}` 从**推送方**拉取内容，
  写入自己的 `data/proxy/<proxy_id>.proxy` 与 `proxy.json`；
  `kind=msg` → 直接把 `text` 收入代理（无需拉取）。
- 应答：`{"cmd":"proxy_ok","proxy_id":"...","ok":true,"mac":"<我的mac>"}`。

### 2.5 `t = "proxy_consumed"` — 代理已被接受

```json
{"t":"proxy_consumed","uid":..,"name":..,"msg_id":..,
 "proxy_id":"...","holder":"<新持有者 mac 或空串>","mac":"<宣布者 mac>","ts":..}
```

- **所有**收到该报文的设备：若本地 `proxy.json` 有该 `proxy_id`：
  - `holder == ""` 或 `holder != 本机 mac` → **删除**本地代理文件与记录；
  - `holder == 本机 mac` → 保留（自己成为唯一分发源）。
- 同时通过 `record_activity("proxy", "...")` 记入活动历史，使得**离线错过的设备在补齐历史时也能执行同样的删除**
  （这就是「当他们从其他用户更新日记信息时，也要对应处理」）。
  活动事件格式固定为：`kind="proxy"`, `text="代理已接受 proxy_id=<pid> holder=<mac>"`。
  `merge_activity` 时解析该文本并执行删除。

### 2.6 代理的接收端行为（最终接受者）

当 `proxy_push` 被接受且 `mac in targets`（即群成员本人收到）：
- 把文件登记进正常文件索引（`has_local=True`，`groups=[gid]`），并广播 `file_pub`（复用 v1.1 机制，
  这样群内其他人按原有「点对点下载」路径获取）；
- 删除自己 `data/proxy/` 里的代理副本吗？**不删**——它已成为 `holder`，作为分发源。
- 日志：`[时间] 已收到群「qqq」的离线文件「作业.txt」（来自设备数据传输代理）`。

当 `mac == sender_uid`（原发送者收到回传）：删除代理（自己本来就有原文件）。

### 2.7 GUI 与日志

所有 2.3–2.6 的日志都写进「日志」页，且 `record_activity("proxy", ...)` 进活动历史。
代理操作**不产生任何 UI 列表项、不弹窗、不可被用户打开**。

---

## 3. 空设备启动提示

### 3.1 判定

「设备空有程序，没有之前局域网相关数据」= 同时满足：

1. `net.json` 不存在，或 `net` 为空/无 `nid`（未加入内网）；
2. `active_log.json` 不存在，或 `events` 为空；
3. `config.json` 的 `groups` 为空。

### 3.2 行为

启动后（GUI 就绪、UDP 已启动）弹出模态对话框：

- 标题：`内部网`
- 正文：`当前没有局域网网络活动，是否新建？`
- 说明行：`未发现局域网内已有的内网与活动数据，你可以新建一个内网并创建第一个群组。`
- 按钮：`是，新建（推荐）` / `否，稍后`

**是** → 关闭对话框，调用 `net_create()`（若扫描到附近内网则先 `net_apply_join`），
然后打开「新建群组」对话框；日志写：
`[时间] 本机没有局域网活动数据，已按用户选择新建内网「<名字>」`

**否** → 关闭对话框，只在日志写：
`[时间] 当前没有局域网网络活动（未新建）`

**有数据时不弹**；此时若本机有活动数据，走第 1 节的 `act_hello` + 补齐流程。

C++ 版用 `MessageBoxW`（`MB_YESNO|MB_ICONQUESTION`）实现，按钮文案用自定义对话框以满足中文按钮文字。

---

## 4. 双更新程序方案

### 4.1 文件

| 角色 | Python | C++ |
| --- | --- | --- |
| 主程序 | `neiwang.py` | `neiwang.exe` |
| 更新程序1 | `neiwang_update1.py` | `neiwang_update1.exe` |
| 更新程序2 | `neiwang_update2.py` | `neiwang_update2.exe` |
| 旧名称兼容 | `neiwang_update.py`（**删除**，由 `更新.bat` 改指向 1） | — |

`neiwang.py` / `neiwang.exe` 启动时自动检查更新，发现新版本时调起 **更新程序1**。

### 4.2 命令行参数（两个更新程序完全一致）

| 参数 | 行为 |
| --- | --- |
| （无） | 手动模式：一定给出明确反馈（已是最新 / 无法连接 / 发现新版本并询问） |
| `--auto` | 静默：只有发现新版本才弹窗 |
| `--update` | 不询问，直接开始更新 |
| `--check` | 无界面，只打印一行：`UPDATE_AVAILABLE <ver>` / `UP_TO_DATE <ver>` / `OFFLINE` |
| `--patch-updater1` | **只属于更新程序2**：不检查、不下载，直接把 `update_clone/` 里的更新程序1 覆盖到程序目录 |

C++ 版额外支持 `--console` 以附加控制台输出（便于测试）。

### 4.3 更新三步（两版一致，沿用 v1.1）

1. `data/` → `customer_god/`（全量备份，先清空 `customer_god/`）；
2. 取新版本到 `update_clone/`：有 `update_repo` 且 git 可用 → `git clone --depth 1`；否则下载
   `update_package` ZIP 并解压（清空 `update_clone/` 后再放）；
3. `customer_god/` → `update_clone/data/`（回填会话内容）。

### 4.4 新增第四步：就地更新全部（更新程序1）

4. 从 `update_clone/` 把**除下列之外的全部文件**覆盖到程序目录（保持相对路径）：
   - `data/`（本机数据已存在，不动）
   - `customer_god/`、`update_clone/`（运行时目录）
   - **更新程序1 自己**（`neiwang_update1.py` / `neiwang_update1.exe`）
   删除在 `update_clone/` 中不存在、但属于「程序文件」的旧文件（白名单见下）：
   `*.py`、`*.exe`、`*.bat`、`update.wenyi`、`README.md`、`*.md`（`docs/` 除外）。
5. 记录 `.update_manifest_applied`（版本号 + 时间），日志 `update_clone.log` 追加。

### 4.5 第五步：更新程序1 更新自己（由更新程序2 完成）

更新程序1 就地更新完成后，用 `--patch-updater1` 调起 **更新程序2**（优先 `update_clone/` 里的新版，
其次程序目录里的）：更新程序2 只做一件事——把 `update_clone/neiwang_update1.*` 复制到程序目录，
覆盖正在运行的更新程序1（此时更新程序1 已退出等待，或通过「重命名旧文件再复制」的方式完成覆盖）。

- Windows 上正在运行的 exe 不能覆盖自身，所以：
  - 更新程序1 用 `CreateProcess` 启动更新程序2 时**带上 `--wait-pid <pid>`**；
  - 更新程序2 先 `WaitForSingleObject` 等更新程序1 退出（超时 30s）；
  - 仍失败则把新文件写成 `neiwang_update1.new.exe` 并提示重启。
- Python 版：`.py` 可被覆盖（解释器只按需读取），同样实现 `--wait-pid` 保持一致。
- 完成日志：`更新程序1 已由更新程序2 更新到 v<ver>`。

### 4.6 目录与清单

```
data/                       用户数据（更新时备份/回填，就地更新时跳过）
customer_god/               备份目录
update_clone/               新版本目录（更新完成后本身也是一个可运行的新版本）
update.wenyi                版本清单（第一行 2.0.0）
update_clone.log            更新日志
_neiwang_update1.pid        更新程序1 的 pid（供更新程序2 等待）
```

### 4.7 `update.wenyi`（v2.0.0）

```
2.0.0
- 全新 C++ 原生版（纯 Win32 + GDI+，零第三方依赖），与 Python 版协议完全兼容、可同网混用
- 新增「局域网活动历史」：启动自动广播物理地址/IP/最后一次数据获取时间，由连续运行最久的设备补齐空档
- 新增「设备数据传输代理」：群内无人在线时把文件或信息临时寄存到局域网在线设备，上线后自动送达并回收
- 新增空设备启动提示「当前没有局域网网络活动，是否新建？」
- 更新程序改为双更新程序：更新程序1 更新除自己外的全部，更新程序2 更新更新程序1
- 保留 v1.1 全部功能：内网准入与入网投票、群聊、私聊、离线保密转存、群文件与离线回退、改名投票、公共日志
```

---

## 5. C++ 版结构（对齐 Python）

```
cpp/
  build.bat                 使用 MSVC vcvars64 编译出全部目标（显式目标文件清单）
  src/common/               json.h/.cpp  sha2.h/.cpp  crypto.h/.cpp  util.h/.cpp
  src/core/                 core.h  constants.cpp  netio.h/.cpp
                            peercore.cpp        （生命周期/广播/在线表/聊天/私聊/改名/日志/更新检查）
                            peercore_net.cpp    （内网准入与入网投票）
                            peercore_file.cpp   （文件索引/下载调度/TCP 服务/保密转存）
                            peercore_activity.cpp（新功能 A：局域网活动历史）
                            peercore_proxy.cpp  （新功能 B：设备数据传输代理）
  src/ui/                   ui_common  notify  forms  app  app_pages  app_actions（Win32 + GDI+）
  src/main.cpp              neiwang.exe
  updater/                  updater_common.h/.cpp + updater1.cpp + updater2.cpp
  tests/                    test_common / test_activity / test_proxy / test_interop
```

- 事件通道与 Python 一致：`EventQueue`（`std::deque<Event>` + 互斥量 + 条件变量），
  `Event{kind, args}`，`args` 是**数组**，与 Python `_emit(kind, *payload)` 一一对应；GUI 线程每 100ms 批量取空。
- 端口、常量、环境变量名与 Python 完全一致（`NEIWANG_*`）。
- **跨语言互通已实测**：`cpp/tests/test_interop.cpp` 驱动 `tools/py_peer.py`（无头 Python 对端），
  覆盖 presence / 内网准入 / 群聊双向 / 公共日志双向 / TCP `act_get` / TCP `get` / 保密转存（跨语言加解密与 HMAC）。
- 两版对齐的两处细节（务必保持一致，否则跨语言校验会对不上）：
  1. `act_have.sig` 的两个浮点一律 `"%.3f"`（Python 用 `f"{x:.3f}"`，C++ 用 `snprintf("%.3f")`）；
  2. 本机对外公布的 IPv4 优先选择**非 169.254.x** 的可路由地址（APIPA 地址对端连不上）。
- 更新程序额外支持 `--root <path>`（Python 通过 `apply_root_arg()` 设置 `NEIWANG_UPDATE_ROOT`，行为一致）。
