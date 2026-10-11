# -*- coding: utf-8 -*-
"""设备数据传输代理（design_v2 第 2 节）无头测试。

覆盖：
  1. proxy_req 在线请求 + proxy_ack 应答 + proxy_store 寄存（真实 UDP + TCP），
     日志文案逐条核对，代理文件写进 data/proxy/<proxy_id>.proxy（用户不可查看）；
  2. TCP cmd=get 扩展 proxy_id：把代理文件按同样格式取回；
  3. proxy_online / presence 引发的「上线提醒」日志 + proxy_on_peer_online 三条分支：
     · 回传原作者（proxy_consumed holder=""）→ 双方删除；
     · 送达目标成员（holder=<target>）→ 推送方删除、接收方保留并登记群文件 + 广播 file_pub；
     · 扩散副本（群成员一直没上线）→ 推送方保留、不 consumed；
  4. t=proxy_consumed：holder 不是本机 -> 删除本地代理文件与记录；
  5. 活动历史里 kind="proxy" 的「代理已接受 proxy_id=<pid> holder=<mac>」记录，
     以及 merge_activity 补齐历史时执行同样的删除；
  6. 无人在线应答时的日志「代理未建立（文件仍保留在本机）」。
不依赖 GUI，用 NEIWANG_DATA_DIR 指向临时目录。
"""
import hashlib
import json
import os
import queue
import shutil
import socket
import sys
import time
from pathlib import Path

os.environ["NEIWANG_DATA_DIR"] = str(Path(__file__).resolve().parent / "_testdata_proxy")
os.environ["NEIWANG_UDP_PORT"] = "45815"
os.environ["NEIWANG_TCP_PORT"] = "45820"
os.environ["NEIWANG_PRES_INTERVAL"] = "1"
os.environ["NEIWANG_PRES_TTL"] = "6"
os.environ["NEIWANG_SCAN_WINDOW"] = "1"
os.environ["NEIWANG_NET_ANNOUNCE"] = "2"
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"

BASE = Path(os.environ["NEIWANG_DATA_DIR"])
if BASE.exists():
    shutil.rmtree(BASE, ignore_errors=True)
BASE.mkdir(parents=True, exist_ok=True)

import neiwang as M

M.PROXY_REQ_WAIT = 2.0        # 测试加速：等待在线应答
UDP = 45815
NID = "PROXYNET"
MAC_A, MAC_B, MAC_C, MAC_D, MAC_E = ("AABBCC001001", "AABBCC001002", "AABBCC001003",
                                     "AABBCC001004", "AABBCC001005")
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


def logs(bucket):
    return [e[1] for e in bucket if e and e[0] == "log"]


def has_log(bucket, needle):
    return wait_for(lambda: any(needle in s for s in logs(bucket)), 20)


def make_core(uid, name, tcp_port, root, nid=NID, members=()):
    root = BASE / root
    root.mkdir(parents=True, exist_ok=True)
    cfg = {"uid": uid, "name": name, "tcp_port": tcp_port, "udp_port": UDP,
           "groups": [], "update_url": "http://127.0.0.1:1/none", "update_page": ""}
    q = queue.Queue()
    core = M.PeerCore(cfg, q, data_root=root)
    core.net = {"nid": nid, "name": "代理测试内网", "founder_mac": uid,
                "created_ts": time.time()}
    core.net_members = {}
    core._net_add_member(uid, name, "127.0.0.1", tcp_port, founder=True)
    for mac, nm, port in members:
        core._net_add_member(mac, nm, "127.0.0.1", port)
    core._save_net()
    return core, q


def tcp_call(core, req: dict, timeout=15):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(("127.0.0.1", core.my_tcp_port()))
        M.send_json(s, req)
        return M.recv_json(s)
    finally:
        s.close()


def fetch_bytes(core, fid, proxy_id=None):
    req = {"cmd": "get", "file_id": fid}
    if proxy_id:
        req["proxy_id"] = proxy_id
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(15)
    try:
        s.connect(("127.0.0.1", core.my_tcp_port()))
        M.send_json(s, req)
        hdr = M.recv_json(s)
        if hdr.get("cmd") != "data":
            return None, hdr
        size = int(hdr.get("size") or 0)
        buf = b""
        while len(buf) < size:
            blk = s.recv(size - len(buf))
            if not blk:
                break
            buf += blk
        return buf, hdr
    finally:
        s.close()


print("=" * 72)
print("设备数据传输代理测试（design_v2 第 2 节）")
print("=" * 72)

print("\n-- 0) 启动 4 台内网成员（同内网、互相当成员） --")
MEMS = ((MAC_A, "AA", 45821), (MAC_B, "BB", 45822), (MAC_C, "CC", 45823), (MAC_D, "DD", 45824))
coreA, qA = make_core(MAC_A, "AA", 45821, "A", members=MEMS[1:])
coreB, qB = make_core(MAC_B, "BB", 45822, "B", members=(MEMS[0], MEMS[2], MEMS[3]))
coreC, qC = make_core(MAC_C, "CC", 45823, "C", members=(MEMS[0], MEMS[1], MEMS[3]))
coreD, qD = make_core(MAC_D, "DD", 45824, "D", members=(MEMS[0], MEMS[1], MEMS[2]))
bucketA, bucketB, bucketC, bucketD = [], [], [], []
for core, q, bucket in ((coreA, qA, bucketA), (coreB, qB, bucketB),
                        (coreC, qC, bucketC), (coreD, qD, bucketD)):
    collect(q, bucket)
for core in (coreB, coreC, coreD):
    core.start()
time.sleep(0.6)
coreA.start()
ok("B/C/D 与 A 相互发现（presence 里有地址与端口）", wait_for(
    lambda: all(u in coreA.peers and coreA.peers[u].get("tcp_port")
                for u in (MAC_B, MAC_C, MAC_D)), 15), sorted(coreA.peers))

# 抑制 presence 自动触发的「上线提醒/送达」，让分支测试完全可控
for core in (coreA, coreB, coreC, coreD):
    core._known_online = {MAC_A, MAC_B, MAC_C, MAC_D}

gid = coreA.create_group("qqq")["gid"]
for core in (coreB, coreC, coreD):
    core.join_group(gid, "qqq")
coreA._group_known.setdefault(gid, {}).update({MAC_B: "BB", MAC_C: "CC"})
time.sleep(0.5)
ok("代理元数据文件路径 = data/proxy.json",
   coreA.proxy_meta_path() == coreA.data_root / "proxy.json")
ok("代理目录 = data/proxy/", coreA.proxy_dir() == coreA.data_root / "proxy")

payload = b"proxy-file-content-" + os.urandom(512)
src = BASE / "作业.txt"
src.write_bytes(payload)
fid = hashlib.sha256(payload).hexdigest()

# ---------------------------------------------------------------- 1) 在线请求 + 寄存 --
print("\n-- 1) proxy_req / proxy_ack / proxy_store（真实 UDP+TCP） --")
pid1 = coreA.start_proxy("file", gid, fid=fid, name="作业.txt", size=len(payload), path=src)
ok("代理 id 是 32 位十六进制随机串",
   len(pid1) == 32 and all(c in "0123456789abcdef" for c in pid1), pid1)
ok("本机代理记录字段齐全（design_v2 2.1）", all(
    k in coreA.proxies[pid1] for k in ("proxy_id", "kind", "gid", "gname", "file_id",
                                       "name", "size", "text", "sender_uid",
                                       "sender_name", "from_mac", "created_ts", "ts",
                                       "targets", "delivered_to", "holders", "consumed",
                                       "holder")))
ok("发起在线请求日志（发送方本机）", has_log(
    bucketA, "「AA」发起在线请求：群「qqq」当前没有其他成员在线，向局域网内所有在线成员请求文件代理"))
ok("应答方回复在线日志", has_log(bucketA, "「BB」（物理地址 AA-BB-CC-00-10-02）回复在线"))
ok("寄存成功日志（发送方）", has_log(
    bucketA, "已向「BB」发送文件代理「作业.txt」（临时寄存，用户不可查看）"))
ok("接收方寄存日志（含 .proxy 后缀说明）", has_log(
    bucketB, "已代「AA」寄存文件代理「作业.txt」（用户不可查看，后缀已改为 .proxy）"))
ok("B 的 proxy.json 里有该代理", wait_for(lambda: pid1 in coreB.proxies, 10))
ok("B 落盘了 data/proxy/<proxy_id>.proxy 且内容一致",
   wait_for(lambda: coreB.proxy_path(pid1).is_file(), 10)
   and coreB.proxy_path(pid1).read_bytes() == payload)
ok("B 的代理记录字段完整且 kind=file / file_id 正确",
   coreB.proxies[pid1]["kind"] == "file"
   and coreB.proxies[pid1]["file_id"] == fid
   and coreB.proxies[pid1]["name"] == "作业.txt"
   and coreB.proxies[pid1]["sender_uid"] == MAC_A
   and coreB.proxies[pid1]["consumed"] is False
   and coreB.proxies[pid1]["holder"] == "")
ok("A 的 holders 里记下了 B", MAC_B in coreA.proxies[pid1]["holders"],
   coreA.proxies[pid1]["holders"])
ok("代理文件后缀固定为 .proxy", coreB.proxy_path(pid1).name == pid1 + ".proxy")
ok("代理操作记进活动历史（kind=proxy）", any(
    e.get("kind") == "proxy" and "已向「BB」发送文件代理" in e.get("text", "")
    for e in coreA.act_events))

# ---------------------------------------------------------------- 2) cmd=get 扩展 --
print("\n-- 2) TCP cmd=get 支持 proxy_id --")
data, hdr = fetch_bytes(coreB, fid, proxy_id=pid1)
ok("带 proxy_id 的 get 能取回代理文件", data == payload and hdr.get("cmd") == "data",
   str(hdr)[:120])
data2, hdr2 = fetch_bytes(coreB, fid, proxy_id="0" * 32)
ok("proxy_id 不匹配时回「没有该文件」", hdr2.get("cmd") == "err", str(hdr2)[:120])

# ---------------------------------------------------------------- 3) 送达目标成员 --
print("\n-- 3) 上线提醒 + 目标成员送达（holder=<target>） --")
coreA._known_online.discard(MAC_C)
pid2 = coreA.start_proxy("file", gid, fid=fid, name="作业.txt", size=len(payload), path=src)
ok("targets 里含群成员 CC（补全后判定 mac in targets）", wait_for(
    lambda: MAC_C in coreA.proxies[pid2]["targets"], 10), coreA.proxies[pid2]["targets"])
coreA._on_peer_online(MAC_C, "CC", [gid])
ok("上线提醒日志", has_log(bucketA, "上线提醒：「CC」（物理地址 AA-BB-CC-00-10-03）已上线"))
ok("送达目标成员日志", has_log(
    bucketA, "群「qqq」成员「CC」已上线，已将文件代理「作业.txt」送达并删除本地代理"))
ok("C 收到离线文件日志（来自设备数据传输代理）", has_log(
    bucketC, "已收到群「qqq」的离线文件「作业.txt」（来自设备数据传输代理）"))
ok("C 保留了代理副本（成为新的分发源）", wait_for(
    lambda: pid2 in coreC.proxies, 10) and coreC.proxies[pid2]["kind"] == "file")
ok("推送方（A）删除了本地代理", wait_for(lambda: pid2 not in coreA.proxies, 10))
ok("C 把文件登记进正常文件索引（has_local + groups）", wait_for(
    lambda: coreC.has_file(fid) and gid in (coreC.index.get(fid, {}).get("groups") or []), 10),
   coreC.index.get(fid))
ok("C 广播了 file_pub（复用 v1.1 机制）", wait_for(
    lambda: any(e[0] == "log" and "在群「qqq」发送了文件「作业.txt」" in e[1]
                for e in bucketB), 15))
ok("活动历史记下「代理已接受 proxy_id=<pid> holder=<CC>」", wait_for(
    lambda: any(e.get("kind") == "proxy"
                and e.get("text") == f"代理已接受 proxy_id={pid2} holder={MAC_C}"
                for e in coreA.act_events), 10),
   [e.get("text") for e in coreA.act_events if e.get("kind") == "proxy"][-3:])
ok("C 侧收到 proxy_consumed 但 holder 是自己 -> 保留",
   pid2 in coreC.proxies and coreC.proxies[pid2]["consumed"] is True
   and coreC.proxies[pid2]["holder"] == MAC_C)

# ---------------------------------------------------------------- 4) 回传原作者 --
print("\n-- 4) 原作者上线 -> 回传并删除（holder=\"\"） --")
# 造一个「B 持有、作者是 A、targets 为空」的代理，专门验证规则 1
pid_ret = coreA.start_proxy("file", gid, fid=fid, name="作业.txt", size=len(payload),
                            path=src)
coreA.proxies[pid_ret]["targets"] = []
ok("B 持有一份 pid_ret 副本", coreA._proxy_push_to(
    {"name": "BB", "ip": "127.0.0.1", "tcp_port": coreB.my_tcp_port()},
    coreA.proxies[pid_ret]) and wait_for(lambda: pid_ret in coreB.proxies, 10))
coreB._known_online.discard(MAC_A)
coreB._on_peer_online(MAC_A, "AA", [])
ok("回传原作者日志", has_log(
    bucketB, "原作者「AA」已上线，已将文件代理「作业.txt」回传并删除本地代理"))
ok("B 删除了本地代理", wait_for(lambda: pid_ret not in coreB.proxies, 10))
ok("B 的代理文件被删除", not coreB.proxy_path(pid_ret).exists())
ok("A 收到 proxy_consumed(holder=\"\") 后也删除",
   wait_for(lambda: pid_ret not in coreA.proxies, 10))
ok("活动历史里有 holder 为空的「代理已接受」记录", wait_for(
    lambda: any(e.get("kind") == "proxy"
                and e.get("text") == f"代理已接受 proxy_id={pid_ret} holder="
                for e in coreB.act_events), 10),
   [e.get("text") for e in coreB.act_events if e.get("kind") == "proxy"][-3:])

# ---------------------------------------------------------------- 5) 扩散副本 --
print("\n-- 5) 群成员一直没上线 -> 扩散副本给局域网其他成员 --")
coreA._known_online.discard(MAC_D)
pid3 = coreA.start_proxy("file", gid, fid=fid, name="作业.txt", size=len(payload), path=src)
coreA.proxies[pid3]["targets"] = []                     # 群里没有任何已知成员
coreA.proxies[pid3]["created_ts"] = time.time() - M.PROXY_GROUP_WAIT - 5
coreA._proxy_no_target.add(pid3)
coreA._on_peer_online(MAC_D, "DD", [])
ok("扩散日志", has_log(
    bucketA, "群「qqq」成员一直未上线，已将文件代理「作业.txt」发送给本局域网成员「DD」一份"))
ok("D 收到了扩散来的代理副本", wait_for(lambda: pid3 in coreD.proxies, 10))
ok("扩散不删除本机代理、不 consumed", pid3 in coreA.proxies
   and coreA.proxies[pid3]["consumed"] is False)
ok("扩散记入 delivered_to / holders",
   MAC_D in coreA.proxies[pid3]["delivered_to"] and MAC_D in coreA.proxies[pid3]["holders"],
   (coreA.proxies[pid3]["delivered_to"], coreA.proxies[pid3]["holders"]))

# ---------------------------------------------------------------- 6) proxy_consumed --
print("\n-- 6) t=proxy_consumed：不是我的 -> 删除本地代理 --")
coreD._on_proxy_consumed_msg({"t": "proxy_consumed", "uid": MAC_A, "name": "AA",
                              "proxy_id": pid3, "holder": MAC_C, "mac": MAC_A,
                              "ts": time.time()})
ok("D 删除本地代理记录", pid3 not in coreD.proxies)
ok("D 删除本地代理文件", not coreD.proxy_path(pid3).exists())
ok("D 把这次删除记进活动历史", any(
    e.get("kind") == "proxy" and e.get("text") == f"代理已接受 proxy_id={pid3} holder={MAC_C}"
    for e in coreD.act_events))

# ---------------------------------------------------------------- 7) 历史合并删除 --
print("\n-- 7) 补齐活动历史时执行同样的删除 --")
coreA._proxy_no_target.discard(pid3)     # 避免再次把 pid3 扩散给 D 干扰断言
coreA._known_online.discard(MAC_D)
pid4 = coreA.start_proxy("file", gid, fid=fid, name="作业.txt", size=len(payload), path=src)
coreA.proxies[pid4]["targets"] = []
coreA.proxies[pid4]["created_ts"] = time.time() - M.PROXY_GROUP_WAIT - 5
coreA._on_peer_online(MAC_D, "DD", [])
ok("D 先拿到 pid4 的副本", wait_for(lambda: pid4 in coreD.proxies, 10))
added = coreD.merge_activity([{
    "eid": "AABBCC001001-9999", "seq": 9999, "ts": time.time(), "uid": MAC_A,
    "name": "AA", "kind": "proxy", "text": f"代理已接受 proxy_id={pid4} holder={MAC_B}"}])
ok("代理事件被合并进历史", len(added) == 1)
ok("合并时执行了同样的删除（记录消失）", pid4 not in coreD.proxies)
ok("合并时执行了同样的删除（文件消失）", not coreD.proxy_path(pid4).exists())

pid5 = coreA.start_proxy("msg", gid, text="离线信息正文")
push_ok = coreA._proxy_push_to({"name": "CC", "ip": "127.0.0.1",
                                "tcp_port": coreC.my_tcp_port()}, coreA.proxies[pid5])
ok("C 先拿到 pid5 的副本", push_ok and wait_for(lambda: pid5 in coreC.proxies, 10))
added2 = coreC.merge_activity([{
    "eid": "AABBCC001001-9998", "seq": 9998, "ts": time.time(), "uid": MAC_A,
    "name": "AA", "kind": "proxy", "text": f"代理已接受 proxy_id={pid5} holder={MAC_C}"}])
ok("holder = 本机 mac 时保留（自己成为唯一分发源）",
   len(added2) == 1 and pid5 in coreC.proxies
   and coreC.proxies[pid5]["consumed"] is True
   and coreC.proxies[pid5]["holder"] == MAC_C)

# ---------------------------------------------------------------- 8) TCP 直接调用 --
print("\n-- 8) TCP proxy_store / proxy_push（消息类代理） --")
resp = tcp_call(coreB, {"cmd": "proxy_store", "proxy_id": "b" * 32, "kind": "msg",
                        "gid": gid, "gname": "qqq", "file_id": "", "name": "", "size": 0,
                        "text": "寄存的信息", "sender_uid": MAC_A, "sender_name": "AA",
                        "targets": [], "ts": time.time(), "port": coreA.my_tcp_port()})
ok("proxy_store 应答 proxy_stored ok", resp.get("cmd") == "proxy_stored"
   and resp.get("ok") is True and resp.get("proxy_id") == "b" * 32, str(resp)[:120])
ok("接收方寄存信息代理日志", has_log(bucketB, "已代「AA」寄存信息代理（用户不可查看）"))
ok("B 的代理记录里存了正文", coreB.proxies["b" * 32]["text"] == "寄存的信息")

push_pid = "c" * 32
resp2 = tcp_call(coreD, {"cmd": "proxy_push", "proxy_id": push_pid, "kind": "msg",
                         "gid": gid, "gname": "qqq", "file_id": "", "name": "", "size": 0,
                         "text": "推送的信息", "sender_uid": MAC_A, "sender_name": "AA",
                         "targets": [MAC_D], "ts": time.time(), "port": coreA.my_tcp_port()})
ok("proxy_push 应答 proxy_ok 且带自己的 mac", resp2.get("cmd") == "proxy_ok"
   and resp2.get("ok") is True and resp2.get("mac") == MAC_D, str(resp2)[:120])
ok("D 存下了推送来的代理", coreD.proxies[push_pid]["text"] == "推送的信息")
ok("D 是 targets 成员 -> 日志「已收到群…的离线信息」", has_log(
    bucketD, "已收到群「qqq」的离线信息（来自设备数据传输代理）"))

# 原作者收到回传：不保留代理
resp3 = tcp_call(coreA, {"cmd": "proxy_push", "proxy_id": "d" * 32, "kind": "msg",
                         "gid": gid, "gname": "qqq", "file_id": "", "name": "", "size": 0,
                         "text": "回传的信息", "sender_uid": MAC_A, "sender_name": "AA",
                         "targets": [], "ts": time.time(), "port": coreB.my_tcp_port()})
ok("原作者收到回传 -> 不保留代理", resp3.get("ok") is True and "d" * 32 not in coreA.proxies)

# ---------------------------------------------------------------- 9) 无应答 --
print("\n-- 9) 局域网内没有其他在线成员 -> 代理未建立 --")
coreE, qE = make_core(MAC_E, "EE", 45825, "E", nid="PROXYNET2")
bucketE = []
collect(qE, bucketE)
coreE.start()
time.sleep(0.8)
old_wait = M.PROXY_REQ_WAIT
M.PROXY_REQ_WAIT = 1.0
try:
    gidE = coreE.create_group("solo")["gid"]
    pidE = coreE.start_proxy("file", gidE, fid=fid, name="作业.txt", size=len(payload),
                             path=src)
    ok("没有任何应答 -> 日志「代理未建立（文件仍保留在本机）」", has_log(
        bucketE, "局域网内没有其他在线成员可以代存，代理未建立（文件仍保留在本机）"))
    ok("代理记录仍留在本机（等成员上线后送达）", pidE in coreE.proxies)
    ok("没有寄存到任何人（holders 只有自己）",
       coreE.proxies[pidE]["holders"] == [MAC_E], coreE.proxies[pidE]["holders"])
finally:
    M.PROXY_REQ_WAIT = old_wait

# ---------------------------------------------------------------- 10) proxy_online --
print("\n-- 10) t=proxy_online 报文引发上线提醒 --")
coreB._known_online.discard(MAC_D)
coreB.peers[MAC_D] = {"name": "DD", "ip": "127.0.0.1", "tcp_port": 45824,
                      "groups": [{"gid": gid, "gname": "qqq"}], "ver": "2.0.0",
                      "ts": time.time()}
bucketB.clear()
coreB._on_proxy_online({"t": "proxy_online", "uid": MAC_D, "name": "DD", "mac": MAC_D,
                        "groups": [gid], "ts": time.time()}, ("127.0.0.1", 45815))
ok("proxy_online -> 上线提醒日志", has_log(
    bucketB, "上线提醒：「DD」（物理地址 AA-BB-CC-00-10-04）已上线"))

# ---------------------------------------------------------------- 收尾 --
for c in (coreA, coreB, coreC, coreD, coreE):
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
