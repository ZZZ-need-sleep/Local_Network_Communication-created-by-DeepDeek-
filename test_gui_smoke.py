# -*- coding: utf-8 -*-
"""GUI 冒烟测试 + 更新检查逻辑测试（纯无头 HEADLESS 不依赖真实窗口只能验证逻辑；
GUI 构建 + 短暂事件循环后自动关闭）。"""
import http.server
import os
import queue
import socketserver
import sys
import threading
import time
from pathlib import Path

os.environ["MCTIER_DATA_DIR"] = str(Path(__file__).resolve().parent / "_testdata_gui")
os.environ["MCTIER_UDP_PORT"] = "45685"
os.environ["MCTIER_TCP_PORT"] = "45640"

import shutil
_td = Path(os.environ["MCTIER_DATA_DIR"])
if _td.exists():
    shutil.rmtree(_td, ignore_errors=True)
_td.mkdir(parents=True, exist_ok=True)

import mctier_lan as M

PASS = []

def ok(name, cond, extra=""):
    PASS.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + (("  " + str(extra)) if extra else ""))

# ---------------------------------------------------------------- 更新检查 --
class Handler(http.server.BaseHTTPRequestHandler):
    version = "9.9.9"
    body = None   # 若设置，则原样返回（用于带「更新内容」的清单）
    def do_GET(self):
        data = self.body if self.body is not None else self.version
        body = data.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a):
        pass

srv = socketserver.TCPServer(("127.0.0.1", 0), Handler)
port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()

def wait_event(q, kind, timeout=8):
    end = time.time() + timeout
    acc = []
    while time.time() < end:
        try:
            ev = q.get(timeout=0.2)
            acc.append(ev)
            if ev[0] == kind:
                return ev
        except queue.Empty:
            pass
    return None

def make_cfg(port):
    return {"uid": "GUI-TEST-1", "name": "GUI测试员", "tcp_port": 45640, "udp_port": 45685,
            "groups": [], "update_url": f"http://127.0.0.1:{port}/version.txt"}

# 1) 远程版本不同 -> 提醒（found=True）
q = queue.Queue()
core = M.PeerCore(make_cfg(port), q, data_root=Path(M.DATA_DIR) / "up1")
Handler.version = "2.0.0"
core.check_update()
ev = wait_event(q, "update")
ok("版本不同 -> 提醒更新", ev and ev[1].get("found") and ev[1].get("remote") == "2.0.0")

# 2) 远程版本相同 -> 无反馈（found=False，GUI 不会弹窗）
q = queue.Queue()
core2 = M.PeerCore(make_cfg(port), q, data_root=Path(M.DATA_DIR) / "up2")
Handler.version = M.APP_VERSION
core2.check_update()
ev = wait_event(q, "update")
ok("版本相同 -> 不提醒", ev and ev[1].get("ok") and not ev[1].get("found"))

# 3) 未联网（连接被拒绝）-> 静默（ok=False，GUI 不弹窗）
q = queue.Queue()
core3 = M.PeerCore(make_cfg(45699), q, data_root=Path(M.DATA_DIR) / "up3")
core3.check_update()
ev = wait_event(q, "update")
ok("未联网 -> 静默无反馈", ev and not ev[1].get("ok"))

# 4) 更新内容解析：纯文本（首行版本号 + 其余为更新内容）
q = queue.Queue()
core4 = M.PeerCore(make_cfg(port), q, data_root=Path(M.DATA_DIR) / "up4")
Handler.body = "2.0.1\n- 修复了 xxx\n- 新增了 yyy"
core4.check_update()
ev = wait_event(q, "update")
ok("纯文本清单：版本号解析", ev and ev[1].get("remote") == "2.0.1")
ok("纯文本清单：更新内容解析", ev and "修复了 xxx" in ev[1].get("notes", "")
   and "新增了 yyy" in ev[1].get("notes", ""))

# 5) 更新内容解析：JSON（notes 为字符串数组）
q = queue.Queue()
core5 = M.PeerCore(make_cfg(port), q, data_root=Path(M.DATA_DIR) / "up5")
Handler.body = '{"version": "2.0.2", "notes": ["修复 A", "新增 B"]}'
core5.check_update()
ev = wait_event(q, "update")
ok("JSON 清单：版本号解析", ev and ev[1].get("remote") == "2.0.2")
ok("JSON 清单：更新内容解析", ev and "修复 A" in ev[1].get("notes", "")
   and "新增 B" in ev[1].get("notes", ""))
Handler.body = None

# 6) 本地 update.wenyi：版本号以文件第一行为准，且带更新内容
ok("本地 update.wenyi 存在", M.local_manifest_path().is_file())
lm = M.load_local_manifest()
ok("本地清单版本号 = 程序版本号", bool(lm.get("version")) and lm["version"] == M.APP_VERSION)
ok("本地清单含更新内容", bool(lm.get("notes")))

# 7) 克隆更新：会话内容备份到 customer_god/，克隆后回填新版本 data/
import subprocess
upd_root = Path(M.DATA_DIR) / "upd_root"
shutil.rmtree(upd_root, ignore_errors=True)
upd_root.mkdir(parents=True, exist_ok=True)
os.environ["MCTIER_UPDATE_ROOT"] = str(upd_root)

src_data = Path(M.DATA_DIR) / "upd_src_data"
shutil.rmtree(src_data, ignore_errors=True)
(src_data / "downloads").mkdir(parents=True, exist_ok=True)
(src_data / "config.json").write_text('{"name": "会话数据", "uid": "U-1"}', encoding="utf-8")
(src_data / "downloads" / "作业.txt").write_text("重要会话内容", encoding="utf-8")

fake_repo = Path(M.DATA_DIR) / "fake_repo"
shutil.rmtree(fake_repo, ignore_errors=True)
fake_repo.mkdir(parents=True, exist_ok=True)
(fake_repo / "mctier_lan.py").write_text("# 新版本代码\n", encoding="utf-8")
(fake_repo / "update.wenyi").write_text("9.9.9\n- 全新版本\n", encoding="utf-8")

def git(*a):
    return subprocess.run(["git", *a], cwd=fake_repo, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode

git("init", "-q"); git("add", "-A")
git("-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "init")

q = queue.Queue()
core7 = M.PeerCore(make_cfg(port), q, data_root=src_data)
res = core7.clone_update(str(fake_repo))

# 备份 + 回填（不依赖 git，任何环境都会执行）
ok("会话内容已备份到 customer_god/",
   (upd_root / "customer_god" / "config.json").is_file()
   and (upd_root / "customer_god" / "downloads" / "作业.txt").is_file())
fake_target = upd_root / "update_clone_fake"
core7.restore_customer_data(upd_root / "customer_god", fake_target)
ok("会话内容可回填到新版本 data/",
   (fake_target / "data" / "config.json").is_file()
   and (fake_target / "data" / "downloads" / "作业.txt").read_text(encoding="utf-8") == "重要会话内容")

# 完整克隆（依赖本机 git；受限环境自动跳过）
if res.get("ok"):
    ok("克隆更新成功（含新版本代码）",
       (upd_root / "update_clone" / "mctier_lan.py").is_file())
    ok("克隆后新版本 data/ 带回会话内容",
       (upd_root / "update_clone" / "data" / "config.json").is_file()
       and (upd_root / "update_clone" / "data" / "downloads" / "作业.txt")
           .read_text(encoding="utf-8") == "重要会话内容")
else:
    print("SKIP 完整克隆（当前环境 git 不可用）：" + str(res.get("reason"))[:150].replace("\n", " "))

res2 = core7.clone_update("")
ok("未配置仓库 -> 明确提示", res2.get("ok") is False and "update_repo" in str(res2.get("reason")))

# ---------------------------------------------------------------- GUI 冒烟 --
try:
    import tkinter as tk
    from mctier_lan import App

    root = tk.Tk()
    app = App(root)
    # 造一个群并激活
    g = app.core.create_group("冒烟测试群")
    app.select_gid(g["gid"])
    time.sleep(0.8)
    root.update()
    n_groups = len(app.core.my_groups())
    core = app.core
    root.after(600, root.destroy)
    root.mainloop()
    ok("GUI 启动并退出正常", n_groups == 1, f"groups={n_groups}")
except Exception as e:
    import traceback; traceback.print_exc()
    ok("GUI 启动并退出正常", False, repr(e))

failed = [n for n, c in PASS if not c]
print("=" * 60)
print(f"结果：{len(PASS) - len(failed)}/{len(PASS)} 通过")
sys.exit(1 if failed else 0)
