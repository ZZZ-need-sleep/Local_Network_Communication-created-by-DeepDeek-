# -*- coding: utf-8 -*-
"""无头协议测试：模拟多台电脑在同一局域网（单机多实例）的行为。

覆盖：
  1. 创建群组 / 加入群组（用户可加入多个群组）
  2. 群组聊天广播（文字）
  3. 发送文件 -> 群成员自动下载（发送人在线 -> 点对点直连）
  4. 发送人离线 -> 公共询问 -> 其他缓存者返回 IP -> 自动从第一个应答者下载
  5. 系统通知内容： 「qqq」群「AA」的「你好.txt」下载完毕
  6. 改名（其他电脑看到的名字更新；第一次自由，之后需投票通过）
  7. 双人私聊（点对点：只有收件人收到，其他人无感）
  8. 保密转存（对方离线 -> 加密转存在线同学 -> 上线自动送达 -> 校验后删除）
"""
import json
import os
import queue
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

# 测试加速：缩短心跳/在线判定/询问等待
os.environ["NEIWANG_PRES_INTERVAL"] = "1"
os.environ["NEIWANG_PRES_TTL"] = "3"
os.environ["NEIWANG_OFFER_WAIT"] = "6"
os.environ["NEIWANG_UDP_PORT"] = "45679"
os.environ["NEIWANG_TCP_PORT"] = "45631"
os.environ["NEIWANG_VOTE_NEED"] = "1"    # 测试环境：同意票超过 1 票（即 2 票）即通过
os.environ["NEIWANG_VOTE_WAIT"] = "8"    # 投票等待 8 秒（超时未过则失败）

import neiwang as M

BASE = Path(__file__).resolve().parent / "_testdata"
import shutil
if BASE.exists():
    shutil.rmtree(BASE, ignore_errors=True)
BASE.mkdir(parents=True, exist_ok=True)
UDP = 45679
PASS = []

def ok(name, cond, extra=""):
    PASS.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + (("  " + str(extra)) if extra else ""))

def wait_for(fn, timeout=15.0, step=0.15):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if fn():
                return True
        except Exception:
            pass
        time.sleep(step)
    return False

def drain(q):
    out = []
    while True:
        try:
            out.append(q.get_nowait())
        except queue.Empty:
            return out

def make_core(uid, name, tcp_port, root):
    root = BASE / root
    root.mkdir(parents=True, exist_ok=True)
    cfg = {"uid": uid, "name": name, "tcp_port": tcp_port, "udp_port": UDP,
           "groups": [], "update_url": "http://127.0.0.1:1/none", "update_page": ""}
    q = queue.Queue()
    core = M.PeerCore(cfg, q, data_root=root)
    return core, q

def collect_events_loop(q, bucket):
    def loop():
        while True:
            try:
                bucket.append(q.get(timeout=0.5))
            except queue.Empty:
                pass
    t = threading.Thread(target=loop, daemon=True)
    t.start()
    return t

def find_index(core, name, timeout=15):
    return wait_for(lambda: any(i.get("name") == name for i in core.index.values()), timeout)

def index_of(core, name):
    for i in core.index.values():
        if i.get("name") == name:
            return i
    return None

def fid_of(core, name):
    for fid, r in core.index.items():
        if r.get("name") == name:
            return fid
    return None


print("=" * 70)
print("内部网 LAN 无头协议测试（4 台模拟电脑）")
print("=" * 70)

# ---------------- 机器 A：创建群组，发送文字与文件 ----------------
coreA, qA = make_core("UID-A", "AA", 45631, "A")
coreB, qB = make_core("UID-B", "BB", 45632, "B")
coreC, qC = make_core("UID-C", "CC", 45633, "C")
bucketA, bucketB, bucketC = [], [], []
collect_events_loop(qA, bucketA); collect_events_loop(qB, bucketB); collect_events_loop(qC, bucketC)

coreA.start(); coreB.start(); coreC.start()
time.sleep(1.5)

# 1) 创建群组并加入
gid = coreA.create_group("qqq")["gid"]
coreB.join_group(gid, "qqq")
coreC.join_group(gid, "qqq")
ok("创建群组", gid in coreA.groups)
ok("多群加入（B、C 各自加入同一群）", gid in coreB.groups and gid in coreC.groups)

# 2) 群聊文字广播
coreA.send_chat(gid, "大家好，我是 AA")
ok("群聊文字接收（B）", wait_for(lambda: any(
    e[0] == "chat" and e[1].get("text") == "大家好，我是 AA" for e in bucketB), 10))
ok("群聊文字接收（C）", wait_for(lambda: any(
    e[0] == "chat" and e[1].get("text") == "大家好，我是 AA" for e in bucketC), 10))

# 3) 发送文件 -> 在线点对点自动下载
payload = (os.urandom(256 * 1024)) + b"hello-group-file"
f1 = BASE / "A_send" / "你好.txt"
f1.parent.mkdir(parents=True, exist_ok=True)
f1.write_bytes(payload)
coreA.send_file_to_group(f1, gid)

ok("B 自动下载(在线直连)", find_index(coreB, "你好.txt", 20))
ok("C 自动下载(在线直连)", find_index(coreC, "你好.txt", 20))
if fid_of(coreB, "你好.txt"):
    ok("B 文件内容一致（SHA256）",
       M.compute_sha256(coreB.cache_dir() / fid_of(coreB, "你好.txt")) == M.compute_sha256(f1))
notifyB = wait_for(lambda: any(
    e[0] == "notify" and e[2] == "「qqq」群「AA」的「你好.txt」下载完毕" for e in bucketB), 10)
ok("B 系统通知格式：「qqq」群「AA」的「你好.txt」下载完毕", notifyB)
ok("B 日志出现「AA」发送文件", wait_for(lambda: any(
    e[0] == "log" and "「AA」在群「qqq」发送了文件「你好.txt」" in e[1] for e in bucketB), 10))

# 4) 改名（其他电脑看到的名字更新）
coreA.rename("AAA改")
ok("改名广播日志", wait_for(lambda: any(
    e[0] == "log" and "「AA」更名为「AAA改」" in e[1] for e in bucketB), 10))

# 4.5) 改名投票：第一次改名后，再次改名需超过 5 名用户同意（测试环境调低为需超过 1 票）
ok("A 已进入投票改名模式", coreA.rename_needs_vote())
try:
    coreA.rename("绕过投票")
    ok("第二次直接改名被拒绝", False)
except PermissionError:
    ok("第二次直接改名被拒绝", True)

pid1 = coreA.start_rename_vote("AA2")
ok("B 收到改名投票请求", wait_for(lambda: any(
    e[0] == "rename_request" and e[1].get("pid") == pid1 for e in bucketB), 10))
ok("C 收到改名投票请求", wait_for(lambda: any(
    e[0] == "rename_request" and e[1].get("pid") == pid1 for e in bucketC), 10))
ok("投票请求内容正确（old/new）", any(
    e[0] == "rename_request" and e[1].get("old") == "AAA改" and e[1].get("new") == "AA2"
    for e in bucketC))

# 同意票不足（B 同意、C 拒绝，仅 1 票）-> 超时失败，名字不变
coreB.cast_rename_vote(pid1, True)
coreC.cast_rename_vote(pid1, False)
ok("同意票不足 -> 改名失败", wait_for(lambda: any(
    e[0] == "rename_vote_done" and e[1] == pid1 and e[2] is False for e in bucketA),
    M.RENAME_VOTE_WAIT + 8))
ok("失败后名字保持不变", coreA.my_name == "AAA改")

# 重新发起：B、C 都同意（重复投票只计一次）-> 超过票数立即通过
pid2 = coreA.start_rename_vote("AA2")
ok("二次发起后 B 再收到请求", wait_for(lambda: any(
    e[0] == "rename_request" and e[1].get("pid") == pid2 for e in bucketB), 10))
ok("二次发起后 C 再收到请求", wait_for(lambda: any(
    e[0] == "rename_request" and e[1].get("pid") == pid2 for e in bucketC), 10))
coreB.cast_rename_vote(pid2, True)
coreB.cast_rename_vote(pid2, True)   # 重复投票应被忽略
coreC.cast_rename_vote(pid2, True)
ok("超过票数 -> 改名成功", wait_for(lambda: any(
    e[0] == "rename_vote_done" and e[1] == pid2 and e[2] is True for e in bucketA), 10))
ok("A 的名字已更新为 AA2", coreA.my_name == "AA2")
ok("B 看到投票通过的改名日志", wait_for(lambda: any(
    e[0] == "log" and "「AAA改」更名为「AA2」" in e[1] and "投票通过" in e[1]
    for e in bucketB), 10))

# 4.6) 双人私聊：A -> B，只有 B 收到，C 收不到；A 本机有回显
coreA.send_dm("UID-B", "B 你好，这是私聊")
ok("B 收到 A 的私聊", wait_for(lambda: any(
    e[0] == "dm" and e[1].get("from_uid") == "UID-A"
    and e[1].get("text") == "B 你好，这是私聊" and not e[1].get("self")
    for e in bucketB), 10))
ok("私聊发送人名字正确（A 已改名为 AA2）", any(
    e[0] == "dm" and e[1].get("from_name") == "AA2" for e in bucketB))
ok("A 本机回显自己的私聊", wait_for(lambda: any(
    e[0] == "dm" and e[1].get("self") and e[1].get("to_uid") == "UID-B"
    for e in bucketA), 10))
coreB.send_dm("UID-A", "AA2 你好，收到")
ok("A 收到 B 的私聊回复", wait_for(lambda: any(
    e[0] == "dm" and e[1].get("from_uid") == "UID-B"
    and e[1].get("text") == "AA2 你好，收到" for e in bucketA), 10))
ok("C 收不到发给 B 的私聊", not any(e[0] == "dm" and not e[1].get("self") for e in bucketC))
ok("发给自己被拒绝", coreA.send_dm("UID-A", "自言自语") is False)

# 4.7) 保密转存：A 给离线的 D 发私聊 -> 加密转存到在线 C -> D 上线自动送达 -> C 删除
coreD, qD = make_core("UID-D", "DD", 45634, "D")   # 先不启动 = 离线
k = os.urandom(32)
ok("转存加解密往返正确", M.secret_decrypt(M.secret_encrypt("完整校验测试", k), k) == "完整校验测试")

# 让 B 暂时下线，确保只有 C 一个在线同学可被选为转存方（结果确定）
coreB.stop()
ok("B 下线", wait_for(lambda: not coreA.is_online("UID-B"), 8))

coreA.send_dm_auto("UID-D", "D 离线时的保密消息")
sent = {}
def got_dm_sent():
    for e in bucketA:
        if e[0] == "dm_sent" and e[1].get("to_uid") == "UID-D":
            sent["v"] = e[1]
            return True
    return False
ok("A 发起离线转存(dm_sent)", wait_for(got_dm_sent, 12))
ok("转存成功且非直发", sent["v"].get("ok") is True and sent["v"].get("direct") is False)
ok("C 已持有转存", wait_for(lambda: bool(coreC._secrets), 6))
rec = next(iter(coreC._secrets.values())) if coreC._secrets else {}
ok("C 端为密文（不含原文）", bool(rec) and rec.get("to_uid") == "UID-D"
   and "D 离线时的保密消息" not in json.dumps(rec, ensure_ascii=False)
   and bool(rec.get("blob")) and bool(rec.get("key")))
ok("C 无感（无任何事件）", not any(e[0] in ("dm", "dm_sent") for e in bucketC))
ok("C 磁盘持久化了转存", (coreC.data_root / "secret_relay.json").exists())

# D 上线 -> C 自动送达 -> D 校验通过 -> C 删除
bucketD = []
collect_events_loop(qD, bucketD)
coreD.start()
ok("D 上线后收到转存私聊", wait_for(lambda: any(
    e[0] == "dm" and e[1].get("from_uid") == "UID-A"
    and e[1].get("text") == "D 离线时的保密消息" and e[1].get("relayed")
    for e in bucketD), 20))
ok("D 校验通过后 C 删除转存", wait_for(lambda: not coreC._secrets, 10))
ok("C 磁盘转存已清空", wait_for(lambda:
    not (coreC.data_root / "secret_relay.json").exists()
    or json.loads((coreC.data_root / "secret_relay.json").read_text(encoding="utf-8")) == {}, 6))
coreD.stop()

# 让 B 重新上线（重建实例并重新加群），供后续离线文件回退测试使用
coreB2, qB2 = make_core("UID-B", "BB", 45632, "B")
bucketB2 = []
collect_events_loop(qB2, bucketB2)
coreB2.start()
coreB2.join_group(gid, "qqq")
coreB = coreB2
bucketB = bucketB2

# 5) 离线回退：文件 C 有、A 已下线、B 未收到原始公告 -> B 加入后公共询问

f2 = BASE / "A_send" / "离线文件.bin"
f2.write_bytes(os.urandom(300 * 1024))
coreA.send_file_to_group(f2, gid)
ok("C 先拿到离线测试文件", find_index(coreC, "离线文件.bin", 20))

# 让 A 下线（先停止，等 B/C 的在线表过期）
coreA.stop()
time.sleep(4.0)
ok("A 被判定离线", wait_for(lambda: not coreB.is_online("UID-A"), 8))

# B 未收到 f2（模拟：B 在这之前刚刚离线；简单起见直接删除 B 的缓存重演）
if fid_of(coreB, "离线文件.bin"):
    coreB.cache_dir().joinpath(fid_of(coreB, "离线文件.bin")).unlink()
    coreB.index.pop(fid_of(coreB, "离线文件.bin"), None)
    coreB._save_index()
# 触发 B 重新同步：C 重播 file_pub（sender 仍为 AA）
coreB._sync_req(gid)
ok("B 从公共询问自动下载(离线回退)", find_index(coreB, "离线文件.bin", 25))
if fid_of(coreB, "离线文件.bin"):
    ok("B 离线回退文件内容一致",
       M.compute_sha256(coreB.cache_dir() / fid_of(coreB, "离线文件.bin")) == M.compute_sha256(f2))
ok("B 离线回退通知显示原作者（A 已改名为「AA2」，应显示「AA2」而非转发者「CC」）", wait_for(lambda: any(
    e[0] == "notify" and "「AA2」的「离线文件.bin」下载完毕" in e[2] for e in bucketB), 15))
ok("B 离线回退通知不含转发者 CC", not any(
    e[0] == "notify" and "「CC」的「离线文件.bin」下载完毕" in e[2] for e in bucketB))
ok("B 日志出现公共询问", wait_for(lambda: any(
    e[0] == "log" and "公共询问文件" in e[1] for e in bucketB), 15))

# 6) 退出群组
coreB.leave_group(gid)
ok("退出群组", gid not in coreB.groups)

# 7) 聊天记录缓存内的系统提示（用于核对通知出现在 GUI）
print("-" * 70)
for name, cond in PASS:
    pass
failed = [n for n, c in PASS if not c]
print("=" * 70)
print(f"结果：{len(PASS) - len(failed)}/{len(PASS)} 通过")
if failed:
    print("失败项：" + ", ".join(failed))
coreB.stop(); coreC.stop()
sys.exit(1 if failed else 0)
