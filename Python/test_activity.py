# -*- coding: utf-8 -*-
"""局域网活动历史（design_v2 第 1 节）无头测试：广播、应答、补齐、合并去重、TCP act_get。

覆盖：
  1. act_hello 广播（启动问询）-> 内网成员 act_have 应答（含实际过网的真实 UDP 往返）；
  2. 请求方逐条写「收到…局域网活动校验数据」日志；
  3. 补齐决策 _act_pick：合格应答选最先到达；退化规则 count 优先、其次 max_ts；
  4. 选中后 TCP act_get 取数、act_data 只含 ts > since 的事件、≤2000 条截断；
  5. 提供方写「已向…提供…」日志；
  6. merge_activity：eid 去重、按 (ts, eid) 排序、seq 不用对方的、first_ts=min、截断；
  7. record_activity / active_log.json 落盘 / activity_view / device_is_empty。
不依赖 GUI，用 NEIWANG_DATA_DIR 指向临时目录。
"""
import json
import os
import queue
import shutil
import socket
import sys
import time
from pathlib import Path

os.environ["NEIWANG_DATA_DIR"] = str(Path(__file__).resolve().parent / "_testdata_act")
os.environ["NEIWANG_UDP_PORT"] = "45795"
os.environ["NEIWANG_TCP_PORT"] = "45790"
os.environ["NEIWANG_PRES_INTERVAL"] = "1"
os.environ["NEIWANG_PRES_TTL"] = "5"
os.environ["NEIWANG_SCAN_WINDOW"] = "1"
os.environ["NEIWANG_NET_ANNOUNCE"] = "2"
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"

BASE = Path(os.environ["NEIWANG_DATA_DIR"])
if BASE.exists():
    shutil.rmtree(BASE, ignore_errors=True)
BASE.mkdir(parents=True, exist_ok=True)

import neiwang as M

M.ACT_PULL_WAIT = 2.0          # 测试加速：等待应答窗口
M.ACT_HELLO_INTERVAL = 3600.0  # 测试期间不再做周期问询
UDP = 45795
NID = "ACTNET"
PASS = []


def ok(name, cond, extra=""):
    PASS.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + (("  " + str(extra)) if extra else ""))


def wait_for(fn, timeout=20.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if fn():
                return True
        except Exception:
            pass
        time.sleep(step)
    return False


def collect(q, bucket):
    import threading

    def loop():
        while True:
            try:
                bucket.append(q.get(timeout=0.5))
            except queue.Empty:
                pass

    threading.Thread(target=loop, daemon=True).start()


def write_log(root: Path, uid: str, events: list, last_get_ts: float, seq: int = 0):
    root.mkdir(parents=True, exist_ok=True)
    (root / "active_log.json").write_text(json.dumps({
        "net_id": NID, "my_mac": uid, "last_get_ts": last_get_ts,
        "first_ts": min([e["ts"] for e in events] or [0]),
        "seq": seq, "events": events,
    }, ensure_ascii=False), encoding="utf-8")


def make_core(uid, name, tcp_port, root, nid=NID):
    root = BASE / root
    root.mkdir(parents=True, exist_ok=True)
    cfg = {"uid": uid, "name": name, "tcp_port": tcp_port, "udp_port": UDP,
           "groups": [], "update_url": "http://127.0.0.1:1/none", "update_page": ""}
    q = queue.Queue()
    core = M.PeerCore(cfg, q, data_root=root)
    core.net = {"nid": nid, "name": "活动测试内网", "founder_mac": uid,
                "created_ts": time.time()}
    core.net_members = {}
    core._net_add_member(uid, name, "127.0.0.1", tcp_port, founder=True)
    core._save_net()
    return core, q


def evt(uid, seq, ts, kind, text):
    return {"eid": f"{uid}-{seq}", "seq": seq, "ts": ts, "uid": uid, "name": uid,
            "kind": kind, "text": text}


print("=" * 72)
print("局域网活动历史测试（design_v2 第 1 节）")
print("=" * 72)

# ------------------------------------------------------------------ 0) 纯函数 --
print("\n-- 0) 校验指纹 / 常量 --")
ok("校验指纹 = sha256(mac|count|last_get_ts|max_ts) 前 16 hex",
   len(M.act_sig("AABB", 3, 1.0, 2.0)) == 16
   and M.act_sig("AABB", 3, 1.0, 2.0) == M.act_sig("AABB", 3, 1.0, 2.0)
   and M.act_sig("AABB", 3, 1.0, 2.0) != M.act_sig("AABB", 4, 1.0, 2.0))
ok("事件上限常量 = 5000", M.ACT_MAX_EVENTS == 5000)
ok("补齐窗口是正数常量", isinstance(M.ACT_PULL_WAIT, float) and M.ACT_PULL_WAIT > 0)

# ------------------------------------------------------------------ 1) 真实过网 --
print("\n-- 1) act_hello 广播 / act_have 应答 / TCP act_get 补齐 --")
T0 = time.time() + 10.0     # 取一个「稍后」的时间点，避免启动时的系统事件混进空档

# A：本机已有历史（ts 都早于 T0），最后数据时间 = T0
a_events = [evt("AABBCC000101", 1, T0 - 300, "log", "AA: 老日志一"),
            evt("AABBCC000101", 2, T0 - 200, "chat", "AA: 老聊天二"),
            evt("AABBCC000101", 3, T0 - 100, "sys", "「AA」启动")]
write_log(BASE / "A", "AABBCC000101", a_events, T0, seq=3)
# B：整晚在线的设备，有 A 缺的新事件
b_new = [evt("AABBCC000202", 4, T0 + 5, "log", "BB: 新日志四"),
         evt("AABBCC000202", 5, T0 + 10, "proxy", "代理已接受 proxy_id=deadbeef holder=AABBCC000202")]
b_events = [evt("AABBCC000202", 1, T0 - 400, "sys", "「BB」启动"),
            evt("AABBCC000202", 2, T0 - 350, "log", "BB: 老日志"),
            evt("AABBCC000202", 3, T0 - 120, "chat", "BB: 老聊天")] + b_new
write_log(BASE / "B", "AABBCC000202", b_events, T0 - 1, seq=5)

coreA, qA = make_core("AABBCC000101", "AA", 45791, "A")
coreB, qB = make_core("AABBCC000202", "BB", 45792, "B")
coreA.last_get_ts = T0
coreB.run_since = T0 - 500          # B 在我最后数据时间之前就一直运行 -> 合格应答
coreA._net_add_member("AABBCC000202", "BB", "127.0.0.1", 45792)
coreB._net_add_member("AABBCC000101", "AA", "127.0.0.1", 45791)

bucketA, bucketB = [], []
collect(qA, bucketA)
collect(qB, bucketB)

print(f"   A 已有 {len(coreA.act_events)} 条，最后数据时间 {M.fmt_time(coreA.last_get_ts)}")
coreB.start()
time.sleep(1.2)
coreA.start()

ok("B 收到 act_hello 并广播应答（应答节流时间戳被写入）",
   wait_for(lambda: coreB._act_have_ts > 0, 10), coreB._act_have_ts)
ok("A 收到应答并逐条写日志（收到…局域网活动校验数据）", wait_for(
    lambda: any(e[0] == "log" and "的局域网活动校验数据" in e[1] for e in bucketA), 15))
ok("应答日志含名字 / 物理地址 / 连续运行自 / 最后数据 / 条数", wait_for(lambda: any(
    e[0] == "log" and "「BB」" in e[1] and M.fmt_mac("AABBCC000202") in e[1]
    and "连续运行自" in e[1] and "最后数据" in e[1]
    and "共 " in e[1] and " 条" in e[1] for e in bucketA), 5),
   [e[1] for e in bucketA if e[0] == "log"][-3:])
ok("A 选定响应最快的 B 构建连接补齐", wait_for(
    lambda: any(e[0] == "log" and "已选定响应最快的「BB」" in e[1]
                and "构建连接补齐本机活动数据" in e[1] for e in bucketA), 15))
ok("A 补齐完成日志（已从「BB」补齐 … 至 … 共 N 条）", wait_for(
    lambda: any(e[0] == "log" and "已从「BB」补齐" in e[1]
                and f"共 {len(b_new)} 条" in e[1] for e in bucketA), 15))
ok("A 的活动历史里出现了 B 的新事件", wait_for(
    lambda: all(any(e.get("eid") == n["eid"] for e in coreA.act_events) for n in b_new), 15),
   [e.get("eid") for e in coreA.act_events][-5:])
ok("A 的 last_get_ts 已更新为当前时间", abs(coreA.last_get_ts - time.time()) < 10)
ok("B 写了「已向…提供…局域网活动信息」日志", wait_for(
    lambda: any(e[0] == "log" and "已向「AA」提供" in e[1] and "的局域网活动信息" in e[1]
                for e in bucketB), 10))
ok("A 落盘了 active_log.json", (coreA.data_root / "active_log.json").is_file())
_disk = json.loads((coreA.data_root / "active_log.json").read_text(encoding="utf-8"))
ok("落盘文件含 net_id / my_mac / last_get_ts / first_ts / seq / events", all(
    k in _disk for k in ("net_id", "my_mac", "last_get_ts", "first_ts", "seq", "events")))
ok("落盘文件里含 B 的新事件文本", any(e.get("text") == "BB: 新日志四" for e in _disk["events"]))
ok("activity_view 条数 = 实际事件数",
   coreA.activity_view()["count"] == len(coreA.act_events), coreA.activity_view())

# ------------------------------------------------------------------ 2) TCP act_get --
print("\n-- 2) TCP cmd=act_get / act_data（只返回 ts > since） --")


def tcp_call(core, req: dict, timeout=10):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(("127.0.0.1", core.my_tcp_port()))
        M.send_json(s, req)
        return M.recv_json(s)
    finally:
        s.close()


resp = tcp_call(coreB, {"cmd": "act_get", "since": T0, "mac": "AABBCC000101", "name": "AA"})
ok("act_get 返回 cmd=act_data", resp.get("cmd") == "act_data" and resp.get("ok") is True,
   str(resp)[:160])
ok("act_data 只含 ts > since 的事件",
   [e["eid"] for e in resp.get("events", [])] == [n["eid"] for n in b_new],
   [e["eid"] for e in resp.get("events", [])])
ok("act_data 含 mac / name / first_ts / last_get_ts / count / events", all(
    k in resp for k in ("mac", "name", "first_ts", "last_get_ts", "count", "events")))
resp0 = tcp_call(coreB, {"cmd": "act_get", "since": T0 + 1000, "mac": "AABBCC000101", "name": "AA"})
ok("since 之后没有事件时返回空 events", resp0.get("cmd") == "act_data"
   and resp0.get("events") == [], str(resp0)[:120])

# ≤2000 条截断（从最早开始，保证空档连续）
saved = coreB.act_events
try:
    big = [evt("BIG000000001", i, T0 + 1000 + i, "log", f"第 {i} 条") for i in range(1, 2501)]
    coreB.act_events = big
    resp2 = tcp_call(coreB, {"cmd": "act_get", "since": T0, "mac": "AABBCC000101", "name": "AA"})
    ok("act_get 最多返回 2000 条", len(resp2.get("events", [])) == 2000,
       len(resp2.get("events", [])))
    ok("截断从最早开始（空档连续）",
       resp2["events"][0]["eid"] == "BIG000000001-1"
       and resp2["events"][-1]["eid"] == "BIG000000001-2000")
finally:
    coreB.act_events = saved

# ------------------------------------------------------------------ 3) 补齐决策 --
print("\n-- 3) 补齐决策 _act_pick --")
coreA.last_get_ts = T0
pick, why = coreA._act_pick([(1.0, {"name": "快", "run_since": T0 - 9, "count": 1, "max_ts": 1}),
                            (2.0, {"name": "覆盖更多", "run_since": T0 - 9, "count": 999,
                                   "max_ts": 9})])
ok("合格应答选最先到达的", pick and pick["name"] == "快" and why == "fastest", (pick, why))
pick2, why2 = coreA._act_pick([
    (1.0, {"name": "晚启动少数据", "run_since": T0 + 100, "count": 5, "max_ts": T0 + 1}),
    (2.0, {"name": "晚启动多数据", "run_since": T0 + 100, "count": 50, "max_ts": T0 + 2}),
    (3.0, {"name": "更晚但更新", "run_since": T0 + 100, "count": 50, "max_ts": T0 + 9})])
ok("没有合格应答时 count 更多者优先（其次 max_ts 更大者）",
   pick2 and pick2["name"] == "更晚但更新" and why2 == "most", (pick2, why2))
pick3, why3 = coreA._act_pick([])
ok("没有任何应答 -> 放弃（记日志）", pick3 is None and why3 == "")

# 没有应答时写「暂无可用于补齐活动数据的设备」日志
bucketA.clear()
coreA._act_no_device_log()
ok("无可用设备日志文案", wait_for(lambda: any(
    e[0] == "log" and "局域网内暂无可用于补齐活动数据的设备" in e[1]
    and "本机最后数据时间" in e[1] for e in bucketA), 5), bucketA[-1:])

# ------------------------------------------------------------------ 4) 合并去重 --
print("\n-- 4) merge_activity 合并规则 --")
before_seq = coreA.act_seq
added = coreA.merge_activity([b_new[0]])          # 已存在 -> 去重
ok("已存在的 eid 不重复合并", added == [])
extra = [evt("CC0000000003", 1, T0 + 30, "log", "CC: 第三条"),
         evt("CC0000000003", 2, T0 + 20, "log", "CC: 第二条")]
added2 = coreA.merge_activity(extra)
ok("新事件被合并", len(added2) == 2, len(added2))
ok("seq 不被对方的序号覆盖", coreA.act_seq == before_seq, coreA.act_seq)
ok("合并后按 (ts, eid) 升序",
   [(e["ts"], e["eid"]) for e in coreA.act_events]
   == sorted([(e["ts"], e["eid"]) for e in coreA.act_events]))
ok("first_ts = 最早事件时间",
   coreA.act_first_ts == min(e["ts"] for e in coreA.act_events), coreA.act_first_ts)
ok("合并后 last_get_ts = 当前时间", abs(coreA.last_get_ts - time.time()) < 5)
ok("合并事件推给 GUI（act_log 事件）", wait_for(lambda: any(
    e[0] == "act_log" and len(e[1]) == 2 for e in bucketA), 5),
   [e[0] for e in bucketA][-3:])
ok("合并事件落盘", any(
    e.get("eid") == "CC0000000003-1"
    for e in json.loads((coreA.data_root / "active_log.json").read_text(
        encoding="utf-8"))["events"]))

# 截断到 ACT_MAX_EVENTS
saved_ev = coreA.act_events
try:
    coreA.act_events = [evt("OLD000000001", i, 1.0 + i, "log", f"x{i}")
                        for i in range(1, M.ACT_MAX_EVENTS + 1)]
    coreA.act_first_ts = 1.0
    coreA.merge_activity([evt("NEW000000001", 1, 9e9, "log", "最新一条")])
    ok("超过 5000 条时丢弃最旧的", len(coreA.act_events) == M.ACT_MAX_EVENTS
       and coreA.act_events[-1]["eid"] == "NEW000000001-1")
finally:
    coreA.act_events = saved_ev

# ------------------------------------------------------------------ 5) 记录 / 空设备 --
print("\n-- 5) record_activity / device_is_empty --")
eid = coreA.record_activity("log", "AA: 手工记录一条")
ok("record_activity 返回 eid = uid-seq", eid == f"{coreA.uid}-{coreA.act_seq}", eid)
coreA.save_activity()
ok("强制落盘后文件里有这条记录", any(
    e.get("eid") == eid
    for e in json.loads((coreA.data_root / "active_log.json").read_text(
        encoding="utf-8"))["events"]))
coreA2 = M.PeerCore({"uid": "DD0000000001", "name": "DD", "tcp_port": 45793,
                     "udp_port": UDP, "groups": [], "update_url": "", "update_page": ""},
                    queue.Queue(), data_root=BASE / "D")
ok("没入网 + 没活动数据 + 没群组 -> 空设备", coreA2.device_is_empty()
   and not coreA2.in_net() and not coreA2.act_events and not coreA2.groups)
coreA2.record_activity("sys", "有了数据")
coreA2.save_activity()
ok("一旦有活动数据就不是空设备", not coreA2.device_is_empty())
ok("本机有内网时不是空设备（用 A 判定）", not coreA.device_is_empty())

# ------------------------------------------------------------------ 收尾 --
for c in (coreA, coreB):
    try:
        c.stop()
    except Exception:
        pass

failed = [n for n, c in PASS if not c]
print("=" * 72)
print(f"结果：{len(PASS) - len(failed)}/{len(PASS)} 通过")
if failed:
    print("失败项：" + ", ".join(failed))
sys.exit(1 if failed else 0)
