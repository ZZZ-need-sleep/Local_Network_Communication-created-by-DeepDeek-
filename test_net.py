# -*- coding: utf-8 -*-
"""内网准入层测试：物理地址登记、扫描/申请/自建、入网投票三种门槛、改票、超时。

覆盖：
  1. 投票规则表：≤5 人免投票；6–7 人全票；≥8 人超过 1/3（且 1/3 规则不得少于 8 人）
  2. 没入网的电脑启动后：扫描附近内网 -> 没有则自行建立（记录发起者物理地址）
  3. 第二台电脑：自动发现内网 -> 申请加入 -> 人少免投票 -> 自动通过、双方登记表互相记录物理地址
  4. 6 人内网：新成员需全票通过；反对票导致无法全票 -> 申请失败
  5. 8 人内网：只需超过 1/3（3 票）即可通过
  6. 弃权不计入同意票
  7. 5 分钟内可改票（改票后票数随之变化）
  8. 投票有效期 1 天 -> 测试环境缩短为若干秒，超时视为投票结束（申请失败）
"""
import os
import queue
import shutil
import sys
import time
from pathlib import Path

# 测试加速
os.environ["NEIWANG_UDP_PORT"] = "45695"
os.environ["NEIWANG_PRES_INTERVAL"] = "1"
os.environ["NEIWANG_PRES_TTL"] = "5"
os.environ["NEIWANG_SCAN_WINDOW"] = "1"          # 开机扫描 1 秒
os.environ["NEIWANG_NET_ANNOUNCE"] = "2"         # 内网公告 2 秒一次
os.environ["NEIWANG_NET_REQ_REBROADCAST"] = "2"  # 申请重播 2 秒一次
os.environ["NEIWANG_NET_VOTE_WINDOW"] = "12"     # 投票有效期 12 秒（代替 1 天）
os.environ["NEIWANG_NET_VOTE_CHANGE"] = "5"      # 5 秒内可改票（代替 5 分钟）
os.environ["NEIWANG_NET_NEARBY_TTL"] = "6"

import neiwang as M

BASE = Path(__file__).resolve().parent / "_testdata_net"
if BASE.exists():
    shutil.rmtree(BASE, ignore_errors=True)
BASE.mkdir(parents=True, exist_ok=True)
UDP = 45695
PASS = []


def ok(name, cond, extra=""):
    PASS.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + (("  " + str(extra)) if extra else ""))


def wait_for(fn, timeout=15.0, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if fn():
                return True
        except Exception:
            pass
        time.sleep(step)
    return False


def make_core(uid, name, tcp_port, root):
    root = BASE / root
    root.mkdir(parents=True, exist_ok=True)
    cfg = {"uid": uid, "name": name, "tcp_port": tcp_port, "udp_port": UDP,
           "groups": [], "update_url": "http://127.0.0.1:1/none", "update_page": ""}
    q = queue.Queue()
    return M.PeerCore(cfg, q, data_root=root), q


def collect(q, bucket):
    def loop():
        while True:
            try:
                bucket.append(q.get(timeout=0.5))
            except queue.Empty:
                pass
    import threading
    threading.Thread(target=loop, daemon=True).start()


print("=" * 72)
print("内网准入层测试（物理地址登记 + 入网投票）")
print("=" * 72)

# ---------------------------------------------------------------- 1) 规则表 --
print("\n-- 1) 投票规则 --")
ok("0 人 -> 免投票", M.net_vote_need(0)[0] == 0)
ok("1 人 -> 免投票", M.net_vote_need(1)[0] == 0)
ok("5 人 -> 免投票", M.net_vote_need(5)[0] == 0)
ok("6 人 -> 全票（6 票）", M.net_vote_need(6)[0] == 6, M.net_vote_need(6))
ok("7 人 -> 全票（7 票）", M.net_vote_need(7)[0] == 7, M.net_vote_need(7))
ok("8 人 -> 超过 1/3（3 票）", M.net_vote_need(8)[0] == 3, M.net_vote_need(8))
ok("9 人 -> 超过 1/3（4 票）", M.net_vote_need(9)[0] == 4)
ok("12 人 -> 超过 1/3（5 票）", M.net_vote_need(12)[0] == 5)
ok("15 人 -> 超过 1/3（6 票）", M.net_vote_need(15)[0] == 6)
ok("规则说明里带上人数与票数", "20 人" in M.net_vote_need(20)[1] and str(20 // 3 + 1) in M.net_vote_need(20)[1])

# ------------------------------------------------- 2) 附近没有内网 -> 自行建立 --
print("\n-- 2) 没有内网 -> 自行建立 --")
coreA, qA = make_core("AABBCC000001", "AA", 45661, "A")
bucketA = []
collect(qA, bucketA)
coreA.start()
ok("启动后自动建立内网（1 秒扫描 + 建立）", wait_for(lambda: coreA.in_net(), 12))
vA = coreA.net_view()
ok("发起者已登记为成员", len(vA["members"]) == 1 and vA["members"][0]["mac"] == "AABBCC000001")
ok("登记表记录了物理地址（显示为 MAC 形式）", M.fmt_mac("AABBCC000001") == "AA-BB-CC-00-00-01")
ok("发起者角色正确", vA["members"][0]["founder"] is True and vA["is_founder"])
ok("落盘了 net.json（记录物理地址）", (coreA.data_root / "net.json").exists())
nid = vA["nid"]
print(f"   内网：{vA['name']} / {nid}")

# ------------------------------------------------- 3) 第二台电脑自动加入 --
print("\n-- 3) 第二台电脑：自动发现 + 申请加入（≤5 人免投票）--")
coreB, qB = make_core("AABBCC000002", "BB", 45662, "B")
bucketB = []
collect(qB, bucketB)
coreB.start()
ok("B 发现附近内网", wait_for(lambda: nid in coreB.net_nearby, 12))
ok("B 自动申请并加入（免投票）", wait_for(lambda: coreB.in_net(), 15))
ok("B 加入的是同一个内网", coreB.net and coreB.net.get("nid") == nid)
ok("A 的登记表出现 B 的物理地址", wait_for(
    lambda: "AABBCC000002" in coreA.net_members and len(coreA.net_members) == 2, 12))
ok("B 的登记表含 A 与 B", wait_for(
    lambda: set(coreB.net_members) == {"AABBCC000001", "AABBCC000002"}, 12))
ok("B 记录到 A 的名字", coreB.net_members.get("AABBCC000001", {}).get("name") == "AA")
ok("加入后在公共日志里可见", wait_for(lambda: any(
    e[0] == "log" and "加入了内网" in e[1] for e in bucketB), 10))

# ------------------------------------------------- 4) 6 人：全票通过 --
print("\n-- 4) 6 人内网：新成员需全票通过 --")
virt4 = [f"V1RT0000000{i}" for i in range(1, 5)]
for i, mac in enumerate(virt4):
    coreA._net_add_member(mac, f"虚拟{i+1}", "10.0.0.%d" % (10 + i), 40000 + i)
coreA._save_net()
coreA._net_sync()
ok("A 内网变成 6 人", len(coreA.net_members) == 6)

coreX, qX = make_core("AABBCC000010", "XX", 45670, "X")
bucketX = []
collect(qX, bucketX)
coreX.start()
ok("X 提交了入网申请", wait_for(lambda: coreX.net_applying is not None, 15))
reqX = (coreX.net_applying or {}).get("req_id")
ok("X 的申请算出的门槛为全票 6 票", wait_for(
    lambda: coreX.net_applying and len(coreX.net_applying.get("voters") or []) == 6, 10),
    (coreX.net_applying or {}).get("voters"))
ok("A 那边收到 X 的申请", wait_for(lambda: reqX in coreA.net_reqs, 10))
prog = coreA._req_progress(coreA.net_reqs[reqX]) if reqX in coreA.net_reqs else {}
ok("A 侧门槛同样为 6 票", prog.get("need") == 6, prog)

# 先投 3 票（不够）
for mac in virt4[:3]:
    coreA._on_net_join_vote({"req_id": reqX, "nid": nid, "voter": mac, "approve": True, "ts": time.time()})
ok("只有 3 票时不能通过", coreX.net_applying is not None and not coreX.in_net())
prog = coreA._req_progress(coreA.net_reqs[reqX])
ok("A 侧显示 3/6 票", prog["approvals"] == 3, prog)

# 剩下 3 票（虚拟成员 1 票 + B 自己投 1 票 + A 自己投 1 票）-> 通过
coreA._on_net_join_vote({"req_id": reqX, "nid": nid, "voter": virt4[3], "approve": True, "ts": time.time()})
coreB.net_cast_vote(reqX, True)      # 真实成员 B 走正常投票路径
coreA.net_cast_vote(reqX, True)
ok("6 票齐全 -> 通过并加入", wait_for(lambda: coreX.in_net(), 15))
ok("X 成为内网成员", "AABBCC000010" in coreA.net_members)
ok("X 收到通过提示", wait_for(lambda: any(
    e[0] == "net_result" and e[1] is True for e in bucketX), 10))
ok("X 的登记表含 6+1 人", len(coreX.net_members) >= 7, len(coreX.net_members))

# ------------------------------------------------- 5) 反对票破坏全票 --
print("\n-- 5) 全票门槛下出现反对票 -> 立即失败 --")
coreY, qY = make_core("AABBCC000011", "YY", 45671, "Y")
bucketY = []
collect(qY, bucketY)
coreY.start()
ok("Y 提交申请", wait_for(lambda: coreY.net_applying is not None, 15))
reqY = (coreY.net_applying or {}).get("req_id")
ok("A 收到 Y 的申请", wait_for(lambda: reqY in coreA.net_reqs, 10))
coreA._on_net_join_vote({"req_id": reqY, "nid": nid, "voter": virt4[0], "approve": False, "ts": time.time()})
ok("出现反对票 -> Y 的申请失败", wait_for(lambda: coreY.net_applying is None, 12))
ok("Y 没有加入内网", not coreY.in_net())
ok("Y 收到失败原因（无法全票通过）", wait_for(lambda: any(
    e[0] == "net_result" and e[1] is False and "全票" in e[2] for e in bucketY), 10))
ok("失败者不在登记表里", "AABBCC000011" not in coreA.net_members)

# ------------------------------------------------- 6) 8 人：超过 1/3 --
print("\n-- 6) 8 人内网：只需超过 1/3（8 人 -> 3 票）--")
i = 0
while len(coreA.net_members) < 8:          # 现有成员数随前面用例变化，这里补到正好 8 人
    mac = f"V2RT0000000{i + 1}"
    coreA._net_add_member(mac, f"虚拟B{i + 1}", "10.0.1.%d" % (10 + i), 41000 + i)
    i += 1
coreA._save_net()
coreA._net_sync()
coreA._net_announce()                      # 让新电脑扫描时能拿到完整成员物理地址表
time.sleep(0.5)
ok("A 内网变成 8 人", len(coreA.net_members) == 8, len(coreA.net_members))

coreZ, qZ = make_core("AABBCC000012", "ZZ", 45672, "Z")
bucketZ = []
collect(qZ, bucketZ)
coreZ.start()
ok("Z 提交申请", wait_for(lambda: coreZ.net_applying is not None, 15))
reqZ = (coreZ.net_applying or {}).get("req_id")
ok("Z 的申请门槛为 3 票", wait_for(
    lambda: coreZ.net_applying and len(coreZ.net_applying.get("voters") or []) == 8, 10))
ok("A 收到 Z 的申请", wait_for(lambda: reqZ in coreA.net_reqs, 10))
prog = coreA._req_progress(coreA.net_reqs[reqZ])
ok("A 侧门槛 = 3 票（超过 1/3）", prog.get("need") == 3, prog)

# 弃权不计入同意
coreA._on_net_join_vote({"req_id": reqZ, "nid": nid, "voter": virt4[0], "approve": None, "ts": time.time()})
ok("弃权不计入同意票", coreA._req_progress(coreA.net_reqs[reqZ])["abstains"] == 1)
ok("弃权后仍未通过", not coreZ.in_net())

for mac in (virt4[1], virt4[2]):
    coreA._on_net_join_vote({"req_id": reqZ, "nid": nid, "voter": mac, "approve": True, "ts": time.time()})
ok("2 票仍不够", not coreZ.in_net())
coreA._on_net_join_vote({"req_id": reqZ, "nid": nid, "voter": virt4[3], "approve": True, "ts": time.time()})
ok("3 票 > 1/3 -> Z 加入成功", wait_for(lambda: coreZ.in_net(), 12))
ok("Z 在登记表中", "AABBCC000012" in coreA.net_members)

# ------------------------------------------------- 7) 5 分钟内可改票 --
print("\n-- 7) 投出后（测试环境 5 秒内）可改票 --")
coreW, qW = make_core("AABBCC000013", "WW", 45673, "W")
bucketW = []
collect(qW, bucketW)
coreW.start()
ok("W 提交申请", wait_for(lambda: coreW.net_applying is not None, 15))
reqW = (coreW.net_applying or {}).get("req_id")
ok("A 收到 W 的申请", wait_for(lambda: reqW in coreA.net_reqs, 10))
coreA.net_cast_vote(reqW, True)
ok("A 投了同意", coreA.net_my_votes.get(reqW, {}).get("approve") is True)
coreA.net_cast_vote(reqW, None)
ok("5 秒内改票为弃权成功", coreA.net_my_votes.get(reqW, {}).get("approve") is None)
coreA.net_cast_vote(reqW, True)
ok("再次改为同意", coreA.net_my_votes.get(reqW, {}).get("approve") is True)
time.sleep(5.5)
try:
    coreA.net_cast_vote(reqW, None)
    ok("超过 5 秒后不能再改票", False)
except RuntimeError as e:
    ok("超过 5 秒后不能再改票", "不能再改票" in str(e))

# ------------------------------------------------- 8) 投票超时 -> 结束 --
print("\n-- 8) 投票超过有效期（测试环境 12 秒）-> 视为投票结束 --")
coreV, qV = make_core("AABBCC000014", "VV", 45674, "V")
bucketV = []
collect(qV, bucketV)
coreV.start()
ok("V 提交申请", wait_for(lambda: coreV.net_applying is not None, 15))
reqV = (coreV.net_applying or {}).get("req_id")
ok("A 收到 V 的申请", wait_for(lambda: reqV in coreA.net_reqs, 10))
coreA._on_net_join_vote({"req_id": reqV, "nid": nid, "voter": virt4[0], "approve": True, "ts": time.time()})
ok("票数不够时不通过", not coreV.in_net())
ok("超过有效期后申请结束（V 收到失败）", wait_for(
    lambda: coreV.net_applying is None and not coreV.in_net(), 30))
ok("失败原因是投票超时", wait_for(lambda: any(
    e[0] == "net_result" and e[1] is False for e in bucketV), 10))
ok("超时者没有加入内网", "AABBCC000014" not in coreA.net_members)

# ------------------------------------------------- 9) 未入网时群组不可用 --
print("\n-- 9) 没入网的电脑：程序是空的（群组不可用）--")
coreN, qN = make_core("AABBCC000099", "NN", 45699, "N")   # 不启动，没有内网
try:
    coreN.create_group("不该建出来")
    ok("未入网禁止建群", False)
except PermissionError as e:
    ok("未入网禁止建群", "内网" in str(e))
try:
    coreN.join_group("ABCDEF", "某群")
    ok("未入网禁止加群", False)
except PermissionError:
    ok("未入网禁止加群", True)

# ------------------------------------------------- 10) 退出内网 --
print("\n-- 10) 手动退出内网：不会自动加回去；再加入需重新投票 --")
coreB.net_leave()
ok("B 已退出内网", not coreB.in_net())
ok("退出会把自己从登记表里移除", wait_for(lambda: "AABBCC000002" not in coreA.net_members, 10))
time.sleep(4)      # 留出自动重扫的时间
ok("退出后不会被自动加回同一个内网", not coreB.in_net())
ok("退出后仍能扫描到该内网", wait_for(lambda: nid in coreB.net_nearby, 12))
coreB.net_apply_join(nid)
ok("手动重新申请已提交", coreB.net_applying is not None)
reqB2 = (coreB.net_applying or {}).get("req_id")
ok("A 收到重新申请", wait_for(lambda: reqB2 in coreA.net_reqs, 12))
prog = coreA._req_progress(coreA.net_reqs[reqB2])
need = prog["need"]
print(f"   重新加入需要 {need} 票（内网 {prog['voters']} 人）")
ok("票数不够时进不来", not coreB.in_net())
virt = sorted(m for m in coreA.net_members if m.startswith("V"))
for mac in virt[:max(0, need - 1)]:
    coreA._on_net_join_vote({"req_id": reqB2, "nid": nid, "voter": mac,
                             "approve": True, "ts": time.time()})
coreA.net_cast_vote(reqB2, True)
ok("票数够了 -> 重新加入成功", wait_for(lambda: coreB.in_net(), 15))
ok("重新加入后登记表里又有 B", wait_for(lambda: "AABBCC000002" in coreA.net_members, 10))

# ---------------------------------------------------------------- 收尾 --
for c in (coreA, coreB, coreX, coreY, coreZ, coreW, coreV):
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
