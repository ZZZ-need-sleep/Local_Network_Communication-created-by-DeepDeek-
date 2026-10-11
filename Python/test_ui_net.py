# -*- coding: utf-8 -*-
"""内网/投票界面断言测试（不依赖肉眼看图：直接检查控件树）：
   · 左侧导航出现「内网 / 投票 / 更多」
   · 有待投票时「投票」图标出现小红点，投完消失
   · 鼠标靠近才出现投票悬浮面板（_show_vote_popup），移开消失
   · 内网页显示成员登记表（物理地址 MAC / 内网 IP），未入网时显示引导页
   · 投票页显示申请卡片（票数 / 门槛 / 剩余时间），点「同意」能投票
   · 「更多」页能看到投票历史
"""
import os
import shutil
import sys
import time
from pathlib import Path

os.environ["NEIWANG_DATA_DIR"] = str(Path(__file__).resolve().parent / "_testdata_ui")
os.environ["NEIWANG_UDP_PORT"] = "45751"
os.environ["NEIWANG_TCP_PORT"] = "45761"
os.environ["NEIWANG_SCAN_WINDOW"] = "1"
os.environ["NEIWANG_NET_ANNOUNCE"] = "2"
os.environ["NEIWANG_NET_VOTE_CHANGE"] = "300"

_td = Path(os.environ["NEIWANG_DATA_DIR"])
shutil.rmtree(_td, ignore_errors=True)
_td.mkdir(parents=True, exist_ok=True)

import tkinter as tk
import neiwang as M

PASS = []


def ok(name, cond, extra=""):
    PASS.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + (("  " + str(extra)) if extra else ""))


def texts(widget):
    """递归收集控件树里所有文本。"""
    out = []
    try:
        t = widget.cget("text")
        if t:
            out.append(str(t))
    except Exception:
        pass
    for c in widget.winfo_children():
        out.extend(texts(c))
    return out


def find_button(widget, label):
    try:
        if widget.winfo_class() == "Button" and str(widget.cget("text")) == label:
            return widget
    except Exception:
        pass
    for c in widget.winfo_children():
        r = find_button(c, label)
        if r is not None:
            return r
    return None


root = tk.Tk()
app = M.App(root)
root.update()

# ---------------------------------------------------------------- 1) 导航 --
print("-- 1) 左侧导航 --")
ok("导航含聊天/内网/投票/更多/日志", set(app.rail_btns) == {"chat", "net", "vote", "more", "log"},
   sorted(app.rail_btns))
ok("投票图标有小红点控件", hasattr(app, "vote_dot"))
ok("刚启动（未入网）聊天页显示内网引导页",
   (lambda: (app.switch_nav("chat"), root.update(), app.view_netgate.winfo_ismapped())[-1])())

# 等自动建立内网
end = time.time() + 20
while time.time() < end and not app.core.in_net():
    root.update()
    time.sleep(0.1)
ok("自动建立内网成功", app.core.in_net())
root.update()

# ---------------------------------------------------------------- 2) 小红点 --
print("\n-- 2) 小红点（有待投票才出现）--")
ok("没有待投票时没有小红点", not app.vote_dot.winfo_ismapped())
ok("没有待投票时点图标不弹面板（内容为空提示）",
   (lambda: (app._show_vote_popup(), root.update(),
             "暂无待我投票" in " ".join(texts(app._vote_popup_body)))[-1])())
app._hide_vote_popup()
root.update()

# 造 6 人内网 + 一条入网申请（6 人 -> 需全票）
for i, (mac, name) in enumerate([("A1B2C3D4E5F6", "李雷"), ("112233445566", "韩梅梅"),
                                 ("AABBCCDDEE01", "王芳"), ("AABBCCDDEE02", "张伟"),
                                 ("AABBCCDDEE03", "刘洋")]):
    app.core._net_add_member(mac, name, f"192.168.1.{20 + i}", 40000 + i)
app.core._save_net()
app.core._net_sync()
app.core._on_net_join_req({
    "t": "net_join_req", "req_id": "ui-req-1", "nid": app.core.net["nid"],
    "mac": "F1E2D3C4B5A6", "name": "赵六", "ip": "192.168.1.88", "tcp_port": 45431,
    "voters": sorted(app.core.net_members), "created_ts": time.time(), "ts": time.time(),
}, ("192.168.1.88", 45455))
app._update_vote_badge()
root.update()
ok("收到入网申请后出现小红点", app.vote_dot.winfo_ismapped())

# ---------------------------------------------------------------- 3) 内网页 --
print("\n-- 3) 内网页（登记表）--")
app.switch_nav("net")
root.update()
rows = [app.net_tree.item(i, "values") for i in app.net_tree.get_children()]
ok("内网页列出成员登记表", len(rows) == 6, len(rows))
mine = [r for r in rows if "（我）" in str(r[0])]
ok("我的物理地址按可读形式显示",
   bool(mine) and str(mine[0][1]) == M.fmt_mac(app.core.uid) and str(mine[0][1]) != "未知",
   mine)
ok("登记表有内网 IP 列", bool(rows) and "192.168" in str(rows[0][2]) or rows[0][2] == "-")
net_texts = " ".join(texts(app.right_net))
ok("内网信息显示成员数与我的物理地址",
   "成员 6 人" in net_texts and "我的物理地址" in net_texts, net_texts[:120])

# ---------------------------------------------------------------- 4) 投票页 --
print("\n-- 4) 投票页 --")
app.switch_nav("vote")
root.update()
vote_texts = " ".join(texts(app.vote_inner))
ok("投票页显示申请人", "赵六" in vote_texts)
ok("投票页显示物理地址", "F1-E2-D3-C4-B5-A6" in vote_texts)
ok("投票页显示票数门槛（6 人全票）", "需 6 票" in vote_texts, vote_texts[:200])
ok("投票页显示规则说明", "全票通过" in vote_texts)
ok("投票页有「同意/拒绝/弃权」三个按钮",
   all(find_button(app.vote_inner, t) is not None for t in ("同意", "拒绝", "弃权")))

# ---------------------------------------------------------------- 5) 悬停弹窗 --
print("\n-- 5) 鼠标靠近才弹出的投票面板 --")
app._show_vote_popup()
root.update()
ok("悬浮面板出现", app._vote_popup is not None and app._vote_popup.winfo_exists())
pop_texts = " ".join(texts(app._vote_popup_body))
ok("面板里有申请卡片（赵六 / 需 6 票）", "赵六" in pop_texts and "需 6 票" in pop_texts)
app._hide_vote_popup()
root.update()
ok("鼠标移开后面板消失", app._vote_popup is None)

# ---------------------------------------------------------------- 6) 投票 --
print("\n-- 6) 投票（同意）--")
btn = find_button(app.vote_inner, "同意")
btn.invoke()
root.update()
ok("投出同意票", app.core.net_my_votes.get("ui-req-1", {}).get("approve") is True)
ok("投完小红点消失", not app.vote_dot.winfo_ismapped())
ok("投票后卡片显示「我已投：同意」", "我已投：同意" in " ".join(texts(app.vote_inner)))
ok("5 分钟内可改票（页面上给出改票按钮）",
   find_button(app.vote_inner, "改拒绝") is not None)

# 票数不够时仍显示"其它进行中的投票"
ok("投票页仍展示该进行中的投票", "赵六" in " ".join(texts(app.vote_inner)))

# ---------------------------------------------------------------- 7) 更多页 --
print("\n-- 7) 更多页（投票历史）--")
app.switch_nav("more")
root.update()
hist = [app.hist_tree.item(i, "values") for i in app.hist_tree.get_children()]
ok("历史里有这条投票", any("赵六" in str(r[1]) for r in hist), hist)
ok("历史里显示我的投票为同意", any("同意" in str(r[3]) for r in hist), hist)
ok("历史里标注可改票", any("可改" in str(r[3]) for r in hist), hist)

# ---------------------------------------------------------------- 8) 收尾 --
end = time.time() + 0.5
while time.time() < end:
    root.update()
    time.sleep(0.05)
app._on_close()

failed = [n for n, c in PASS if not c]
print("=" * 66)
print(f"结果：{len(PASS) - len(failed)}/{len(PASS)} 通过")
if failed:
    print("失败项：" + ", ".join(failed))
sys.exit(1 if failed else 0)
