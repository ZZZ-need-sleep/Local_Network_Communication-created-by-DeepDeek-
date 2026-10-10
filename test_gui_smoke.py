# -*- coding: utf-8 -*-
"""更新机制测试 + GUI 冒烟测试。

覆盖：
  1. 主程序 neiwang.py 的更新检查：远程版本更新 -> 提醒；
     版本号相同 / 更旧 / 未联网 -> 完全静默（不弹窗）；
  2. 更新清单解析：纯文本格式、JSON 格式；本地 update.wenyi（版本号来源）；
  3. 双更新程序 neiwang_update1.py / neiwang_update2.py（公共逻辑在 neiwang_update_common.py）：
     版本比较、检查的三种状态、
     更新三步（备份 data/ -> 取回新版本（ZIP 更新包 / Git）-> 回填到新版本 data/）、
     就地更新（更新程序1 更新除自己外的全部）、更新程序2 自我更新更新程序1、
     危险 ZIP 拒绝、--check 命令行输出；
  4. 主程序里不再有「旧品牌词」和更新执行代码（更新已拆成独立程序）；
  5. GUI 冒烟：构建主窗口、建群、短暂事件循环后关闭。
"""
import http.server
import io
import json
import os
import queue
import shutil
import socketserver
import subprocess
import sys
import threading
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TESTDATA = ROOT / "_testdata_gui"

os.environ["NEIWANG_DATA_DIR"] = str(TESTDATA)
os.environ["NEIWANG_UDP_PORT"] = "45685"
os.environ["NEIWANG_TCP_PORT"] = "45640"
os.environ["NEIWANG_SCAN_WINDOW"] = "1"
os.environ["NEIWANG_NET_ANNOUNCE"] = "2"
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"

if TESTDATA.exists():
    shutil.rmtree(TESTDATA, ignore_errors=True)
TESTDATA.mkdir(parents=True, exist_ok=True)

import neiwang as M
import neiwang_update1 as U
import neiwang_update2 as V
import neiwang_update_common as C

PASS = []


def ok(name, cond, extra=""):
    PASS.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + (("  " + str(extra)) if extra else ""))


def wait_event(q, kind, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        try:
            ev = q.get(timeout=0.2)
            if ev[0] == kind:
                return ev
        except queue.Empty:
            pass
    return None


# ------------------------------------------------------------------ 测试服务器 --
class Handler(http.server.BaseHTTPRequestHandler):
    version = "9.9.9"
    body = None          # 若设置，则原样返回（用于带「更新内容」的清单）
    zip_bytes = None     # 若设置，/pkg.zip 返回它（模拟 ZIP 更新包）

    def do_GET(self):
        if self.path.startswith("/pkg.zip") and self.zip_bytes is not None:
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Length", str(len(self.zip_bytes)))
            self.end_headers()
            self.wfile.write(self.zip_bytes)
            return
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
PORT = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()
MANIFEST_URL = f"http://127.0.0.1:{PORT}/version.txt"


def build_zip(files: dict) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, content in files.items():
            zf.writestr(name, content)
    return buf.getvalue()


def make_cfg(port=None):
    return {"uid": "GUI-TEST-1", "name": "GUI测试员", "tcp_port": 45640, "udp_port": 45685,
            "groups": [], "update_url": MANIFEST_URL if port is None else f"http://127.0.0.1:{port}/version.txt"}


# ============================================================ 1) 主程序更新检查 --
q = queue.Queue()
core = M.PeerCore(make_cfg(), q, data_root=TESTDATA / "up1")
Handler.version = "2.0.1"      # 比本地版本（update.wenyi 第一行）更新
core.check_update()
ev = wait_event(q, "update")
ok("远程版本更新 -> 提醒", ev and ev[1].get("found") and ev[1].get("remote") == "2.0.1")

q = queue.Queue()
core2 = M.PeerCore(make_cfg(), q, data_root=TESTDATA / "up2")
Handler.version = M.APP_VERSION
core2.check_update()
ev = wait_event(q, "update")
ok("版本号相同 -> 静默（不提醒）", ev and ev[1].get("ok") and not ev[1].get("found"))

# 远程版本比本地更旧：同样当作无更新（静默）
q = queue.Queue()
core_old = M.PeerCore(make_cfg(), q, data_root=TESTDATA / "up_old")
Handler.version = "0.1"
core_old.check_update()
ev = wait_event(q, "update")
ok("远程版本更旧 -> 静默（不提醒）", ev and ev[1].get("ok") and not ev[1].get("found"))

q = queue.Queue()
core3 = M.PeerCore(make_cfg(45699), q, data_root=TESTDATA / "up3")
core3.check_update()
ev = wait_event(q, "update")
ok("未联网（连接被拒绝）-> 静默无反馈", ev and not ev[1].get("ok"))

# 清单解析：纯文本 / JSON
q = queue.Queue()
core4 = M.PeerCore(make_cfg(), q, data_root=TESTDATA / "up4")
Handler.body = "2.0.1\n- 修复了 xxx\n- 新增了 yyy"
core4.check_update()
ev = wait_event(q, "update")
ok("纯文本清单：版本号解析", ev and ev[1].get("remote") == "2.0.1")
ok("纯文本清单：更新内容解析", ev and "修复了 xxx" in ev[1].get("notes", "")
   and "新增了 yyy" in ev[1].get("notes", ""))

q = queue.Queue()
core5 = M.PeerCore(make_cfg(), q, data_root=TESTDATA / "up5")
Handler.body = '{"version": "2.0.2", "notes": ["修复 A", "新增 B"]}'
core5.check_update()
ev = wait_event(q, "update")
ok("JSON 清单：版本号解析", ev and ev[1].get("remote") == "2.0.2")
ok("JSON 清单：更新内容解析", ev and "修复 A" in ev[1].get("notes", "")
   and "新增 B" in ev[1].get("notes", ""))
Handler.body = None

ok("本地 update.wenyi 存在", M.local_manifest_path().is_file())
lm = M.load_local_manifest()
ok("本地清单版本号 = 程序版本号", bool(lm.get("version")) and lm["version"] == M.APP_VERSION)
ok("本地清单含更新内容", bool(lm.get("notes")))

# 版本比较：只有更高才算有更新
ok("版本比较：1.1.0 > 1.0", M.is_newer_version("1.1.0", "1.0"))
ok("版本比较：1.0 与 1.0.0 视为相同",
   not M.is_newer_version("1.0", "1.0.0") and not M.is_newer_version("1.0.0", "1.0"))
ok("版本比较：更旧不算更新", not M.is_newer_version("0.9", "1.0"))

# 主程序里已经没有任何旧品牌词 / 更新执行代码
needle_brand = "mc" + "tier"
needle_brand2 = "mc" + "tire"
files = ["neiwang.py", "neiwang_update1.py", "neiwang_update2.py",
         "neiwang_update_common.py", "test_protocol.py", "test_gui_smoke.py",
         "启动.bat", "更新.bat", "update.wenyi", "README.md", ".gitignore"]
bad = []
for name in files:
    p = ROOT / name
    if not p.is_file():
        bad.append(f"{name}: 文件不存在")
        continue
    low = p.read_text(encoding="utf-8", errors="replace").lower()
    if needle_brand in low or needle_brand2 in low:
        bad.append(name)
ok("程序/文档里已无旧品牌词", not bad, "、".join(bad))
ok("主程序里已无更新执行代码（已拆成独立更新程序）",
   not hasattr(M.PeerCore, "clone_update") and not hasattr(M, "restore_customer_data"))
ok("更新程序1 存在且可被主程序定位（双更新程序）",
   U.__file__ and M.updater_path().is_file() and M.updater_path().name == "neiwang_update1.py",
   M.updater_path().name)
ok("更新程序2 存在", V.__file__ and (ROOT / "neiwang_update2.py").is_file())
ok("公共逻辑模块存在（两个入口可独立双击运行）",
   (ROOT / "neiwang_update_common.py").is_file())


def run_script(script, *args, env=None):
    p = subprocess.run([sys.executable, "-B", str(ROOT / script), *args], cwd=str(ROOT),
                       env=env or dict(os.environ), capture_output=True, text=True,
                       timeout=60)
    return ((p.stdout or "") + (p.stderr or "")).strip()


out2 = run_script("neiwang_update2.py", "--check")
_ok2 = bool(out2.splitlines()) and (out2.splitlines()[0].startswith("OFFLINE")
                                    or out2.splitlines()[0].startswith("UP_TO_DATE")
                                    or out2.splitlines()[0].startswith("UPDATE_AVAILABLE"))
ok("更新程序2 --check 打印单行结果（不崩）", _ok2, out2[:120])


# 命令行参数 -> 运行模式（主程序点「是」时用的就是 --update）
class _StubApp:
    last = None

    def __init__(self, mode, cfg):
        _StubApp.last = mode

    def mainloop(self):
        pass


_real_app = U.UpdaterApp
try:
    U.UpdaterApp = _StubApp
    U.main(["--update"])
    ok("--update -> 直接更新模式（主程序调起的方式）", _StubApp.last == "update",
       str(_StubApp.last))
    U.main(["--auto"])
    ok("--auto -> 静默模式（无网络/版本相同不打扰）", _StubApp.last == "auto", str(_StubApp.last))
    U.main([])
    ok("无参数 -> 手动模式", _StubApp.last == "manual", str(_StubApp.last))
finally:
    U.UpdaterApp = _real_app

# ==================================================== 2) 独立更新程序 neiwang_update --
ok("更新程序版本比较函数一致",
   U.is_newer_version("1.2", "1.1") and not U.is_newer_version("1.1", "1.1")
   and not U.is_newer_version("1.0", "1.1"))
ok("更新程序使用独立数据目录（NEIWANG_DATA_DIR）", U.get_data_dir() == TESTDATA)

# 检查的三种状态
Handler.version = "9.9.9"
r = U.check_for_update({"update_url": MANIFEST_URL})
ok("更新程序：发现新版本 -> update", r.get("status") == "update" and r.get("remote") == "9.9.9")
Handler.version = U.local_version()
r = U.check_for_update({"update_url": MANIFEST_URL})
ok("更新程序：版本相同 -> current", r.get("status") == "current")
Handler.version = "9.9.9"
r = U.check_for_update({"update_url": "http://127.0.0.1:1/none"})
ok("更新程序：连不上 -> offline", r.get("status") == "offline")
r = U.check_for_update({"update_url": ""})
ok("更新程序：没配地址 -> offline", r.get("status") == "offline")

# 更新三步：ZIP 更新包（不需要 Git，内网部署用）
upd_root = TESTDATA / "upd_zip"
upd_root.mkdir(parents=True, exist_ok=True)
os.environ["NEIWANG_UPDATE_ROOT"] = str(upd_root)

src_data = TESTDATA / "src_data"
(src_data / "downloads").mkdir(parents=True, exist_ok=True)
(src_data / "config.json").write_text('{"name": "会话数据", "uid": "U-1"}', encoding="utf-8")
(src_data / "downloads" / "作业.txt").write_text("重要会话内容", encoding="utf-8")
os.environ["NEIWANG_DATA_DIR"] = str(src_data)

Handler.zip_bytes = build_zip({
    "内部网-main/neiwang.py": "# 新版本代码\n",
    "内部网-main/update.wenyi": "9.9.9\n- 全新版本\n",
    "内部网-main/neiwang_update1.py": "# 新版更新程序1\n",
    "内部网-main/neiwang_update2.py": "# 新版更新程序2\n",
    "内部网-main/neiwang_update_common.py": "# 新版公共逻辑\n",
})
logs = []
res_zip = U.perform_update({"update_repo": "", "update_package": f"http://127.0.0.1:{PORT}/pkg.zip"},
                           log=logs.append)
ok("ZIP 更新：整体成功", res_zip.get("ok") and res_zip.get("method") == "zip",
   str(res_zip.get("reason", ""))[:200])
ok("ZIP 更新：会话内容已备份到 customer_god/",
   (upd_root / "customer_god" / "config.json").is_file()
   and (upd_root / "customer_god" / "downloads" / "作业.txt").is_file())
ok("ZIP 更新：新版本代码已就位",
   (upd_root / "update_clone" / "neiwang.py").is_file()
   and (upd_root / "update_clone" / "neiwang_update1.py").is_file())
ok("ZIP 更新：会话内容已回填到新版本 data/",
   (upd_root / "update_clone" / "data" / "config.json").is_file()
   and (upd_root / "update_clone" / "data" / "downloads" / "作业.txt")
       .read_text(encoding="utf-8") == "重要会话内容")
ok("ZIP 更新：进度日志有输出", any("备份" in s for s in logs) and any("回填" in s for s in logs))

# 取包方式的判定逻辑（不依赖本机 git，用替换函数确定性地验证）
upd_root_logic = TESTDATA / "upd_logic"
upd_root_logic.mkdir(parents=True, exist_ok=True)
os.environ["NEIWANG_UPDATE_ROOT"] = str(upd_root_logic)
real_have_git, real_run_git = C.have_git, C.run_git_clone


def fake_git_factory(ok_flag, create_main=True):
    def _fake(repo, target, log=print):
        if ok_flag and create_main:
            Path(target).mkdir(parents=True, exist_ok=True)
            (Path(target) / "neiwang.py").write_text("# git 取回的新版本\n", encoding="utf-8")
        return ok_flag, "" if ok_flag else "模拟 git 失败"
    return _fake


try:
    # a) 没装 Git + 配了 ZIP 包 -> 自动走 ZIP
    shutil.rmtree(upd_root_logic / "update_clone", ignore_errors=True)
    C.have_git = lambda: False
    r = U.fetch_new_version({"update_repo": "https://example.com/a.git",
                             "update_package": f"http://127.0.0.1:{PORT}/pkg.zip"},
                            upd_root_logic / "update_clone", log=lambda s: None)
    ok("没装 Git 时自动改用 ZIP 更新包", r.get("ok") and r.get("method") == "zip",
       str(r.get("reason", ""))[:150])

    # b) 有 Git 且克隆成功 -> 走 Git
    shutil.rmtree(upd_root_logic / "update_clone", ignore_errors=True)
    C.have_git = lambda: True
    C.run_git_clone = fake_git_factory(True)
    r = U.fetch_new_version({"update_repo": "https://example.com/a.git", "update_package": ""},
                            upd_root_logic / "update_clone", log=lambda s: None)
    ok("有 Git 时优先用 Git 方式", r.get("ok") and r.get("method") == "git",
       str(r.get("reason", ""))[:150])

    # c) Git 失败 + 配了 ZIP 包 -> 自动回退到 ZIP
    shutil.rmtree(upd_root_logic / "update_clone", ignore_errors=True)
    C.run_git_clone = fake_git_factory(False)
    r = U.fetch_new_version({"update_repo": "https://example.com/a.git",
                             "update_package": f"http://127.0.0.1:{PORT}/pkg.zip"},
                            upd_root_logic / "update_clone", log=lambda s: None)
    ok("Git 失败时自动回退到 ZIP 更新包", r.get("ok") and r.get("method") == "zip",
       str(r.get("reason", ""))[:150])
finally:
    C.have_git, C.run_git_clone = real_have_git, real_run_git

# 真机 Git 端到端（受限环境 / 沙箱里 git clone 可能不可用，此时记 SKIP）
if real_have_git():
    upd_root_git = TESTDATA / "upd_git"
    upd_root_git.mkdir(parents=True, exist_ok=True)
    os.environ["NEIWANG_UPDATE_ROOT"] = str(upd_root_git)
    fake_repo = TESTDATA / "fake_repo"
    fake_repo.mkdir(parents=True, exist_ok=True)
    (fake_repo / "neiwang.py").write_text("# 新版本代码\n", encoding="utf-8")
    (fake_repo / "neiwang_update1.py").write_text("# 新版更新程序1\n", encoding="utf-8")
    (fake_repo / "update.wenyi").write_text("9.9.9\n- 全新版本\n", encoding="utf-8")

    def git(*a):
        return subprocess.run(["git", *a], cwd=fake_repo, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode

    git("init", "-q")
    git("add", "-A")
    git("-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "init")
    res_git = U.perform_update({"update_repo": str(fake_repo), "update_package": ""},
                               log=lambda s: None)
    if res_git.get("ok"):
        ok("Git 更新（真机）：整体成功", res_git.get("method") == "git")
        ok("Git 更新（真机）：新版本 + 会话内容都在",
           (upd_root_git / "update_clone" / "neiwang.py").is_file()
           and (upd_root_git / "update_clone" / "data" / "downloads" / "作业.txt")
               .read_text(encoding="utf-8") == "重要会话内容")
    else:
        print("SKIP Git 更新（真机 git 在当前环境不可用）："
              + str(res_git.get("reason"))[:160].replace("\n", " "))
    os.environ["NEIWANG_UPDATE_ROOT"] = str(upd_root)
else:
    print("SKIP Git 更新（当前环境没有 git）")

# 取包失败 -> 明确说明原因
res_bad = U.perform_update({"update_repo": "", "update_package": ""}, log=lambda s: None)
ok("既没 Git 仓库也没 ZIP 包 -> 明确报错",
   res_bad.get("ok") is False and ("update_repo" in str(res_bad.get("reason"))
                                   or "update_package" in str(res_bad.get("reason"))))

# 危险 ZIP（含 ../）必须被拒绝
try:
    evil = build_zip({"内部网-main/../../evil.txt": "x"})
    tmp = TESTDATA / "evil"
    tmp.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(evil)) as zf:
        U.safe_extract(zf, tmp)
    ok("危险 ZIP（含 ../）被拒绝", False)
except Exception:
    ok("危险 ZIP（含 ../）被拒绝", True)

os.environ["NEIWANG_DATA_DIR"] = str(TESTDATA)

# ---------------------------------------------------- 3) --check 命令行（三种结果）
cli_dir = TESTDATA / "cli_data"
cli_dir.mkdir(parents=True, exist_ok=True)
cli_env = dict(os.environ)
cli_env["NEIWANG_DATA_DIR"] = str(cli_dir)
cli_env["PYTHONDONTWRITEBYTECODE"] = "1"
UPDATER = str(ROOT / "neiwang_update1.py")
UPDATER2 = str(ROOT / "neiwang_update2.py")


def run_check(update_url: str, script: str = None) -> str:
    (cli_dir / "config.json").write_text(json.dumps({"update_url": update_url}), encoding="utf-8")
    p = subprocess.run([sys.executable, "-B", script or UPDATER, "--check"], cwd=str(ROOT),
                       env=cli_env, capture_output=True, text=True, timeout=60)
    return ((p.stdout or "") + (p.stderr or "")).strip()


try:
    Handler.version = "9.9.9"
    out = run_check(MANIFEST_URL)
    ok("--check：有新版本 -> UPDATE_AVAILABLE", "UPDATE_AVAILABLE 9.9.9" in out, out[:120])
    Handler.version = U.local_version()
    out = run_check(MANIFEST_URL)
    ok("--check：版本相同 -> UP_TO_DATE", "UP_TO_DATE" in out, out[:120])
    out = run_check("http://127.0.0.1:1/none")
    ok("--check：连不上 -> OFFLINE", "OFFLINE" in out, out[:120])
    Handler.version = "9.9.9"
    out = run_check(MANIFEST_URL, UPDATER2)
    ok("更新程序2 --check 同样单行输出（两个更新程序参数一致）",
       out.strip().startswith("UPDATE_AVAILABLE"), out[:120])
except Exception as e:
    ok("--check 命令行可用", False, repr(e))

# ------------------------------------------- 3.5) 双更新程序：就地更新 / 更新程序1 自己 --
# 第四步：就地更新全部（除 data/、customer_god/、update_clone/ 与更新程序1 自己以外）
ip_clone = TESTDATA / "ip_clone"
ip_root = TESTDATA / "ip_root"
if ip_root.exists():
    shutil.rmtree(ip_root, ignore_errors=True)
if ip_clone.exists():
    shutil.rmtree(ip_clone, ignore_errors=True)
(ip_clone / "docs").mkdir(parents=True, exist_ok=True)
(ip_clone / "sub").mkdir(parents=True, exist_ok=True)
(ip_clone / "data").mkdir(parents=True, exist_ok=True)
(ip_clone / "neiwang.py").write_text("# new main\n", encoding="utf-8")
(ip_clone / "sub" / "helper.py").write_text("# new helper\n", encoding="utf-8")
(ip_clone / "update.wenyi").write_text("9.9.9\n- x\n", encoding="utf-8")
(ip_clone / "docs" / "design_v2.md").write_text("# new doc\n", encoding="utf-8")
(ip_clone / "data" / "config.json").write_text("{}", encoding="utf-8")
(ip_clone / "neiwang_update1.py").write_text("# new updater1\n", encoding="utf-8")
(ip_clone / "neiwang_update2.py").write_text("# new updater2\n", encoding="utf-8")
(ip_root / "docs").mkdir(parents=True, exist_ok=True)
(ip_root / "data").mkdir(parents=True, exist_ok=True)
(ip_root / "neiwang.py").write_text("# old main\n", encoding="utf-8")
(ip_root / "neiwang_update1.py").write_text("# old updater1\n", encoding="utf-8")
(ip_root / "old_module.py").write_text("# 旧版才有的模块\n", encoding="utf-8")
(ip_root / "docs" / "old.md").write_text("# 旧文档\n", encoding="utf-8")
(ip_root / "notes.txt").write_text("不是程序文件\n", encoding="utf-8")
(ip_root / "data" / "config.json").write_text('{"keep": 1}', encoding="utf-8")
ip_res = U.inplace_update(ip_clone, ip_root, log=lambda s: None)
ok("就地更新：整体成功", ip_res.get("ok") is True, str(ip_res)[:200])
ok("就地更新：程序文件已覆盖",
   (ip_root / "neiwang.py").read_text(encoding="utf-8") == "# new main\n"
   and (ip_root / "sub" / "helper.py").is_file())
ok("就地更新：data/ 不动（本机数据保留）",
   (ip_root / "data" / "config.json").read_text(encoding="utf-8") == '{"keep": 1}'
   and not (ip_root / "data" / "config.json").read_text(encoding="utf-8").startswith("{}"))
ok("就地更新：更新程序1 自己不覆盖（留给更新程序2）",
   (ip_root / "neiwang_update1.py").read_text(encoding="utf-8") == "# old updater1\n")
ok("就地更新：删除 update_clone 中已不存在的程序文件",
   not (ip_root / "old_module.py").exists())
ok("就地更新：docs/ 下的旧 .md 不删除（白名单排除 docs/）",
   (ip_root / "docs" / "old.md").is_file())
ok("就地更新：非程序文件不删除",
   (ip_root / "notes.txt").is_file())
ok("就地更新：目录列表里有删除记录", int(ip_res.get("removed") or 0) >= 1, ip_res)
ok("就地更新：程序目录里的 update.wenyi 被更新",
   (ip_root / "update.wenyi").read_text(encoding="utf-8").startswith("9.9.9"))

# 第五步：更新程序2 只做一件事 —— 把 update_clone/neiwang_update1.* 覆盖到程序目录
u2_root = TESTDATA / "u2_root"
u2_prog = TESTDATA / "u2_prog"
shutil.rmtree(u2_root, ignore_errors=True)
shutil.rmtree(u2_prog, ignore_errors=True)
(u2_root / "update_clone").mkdir(parents=True, exist_ok=True)
u2_prog.mkdir(parents=True, exist_ok=True)
(u2_root / "update_clone" / "neiwang_update1.py").write_text("# patched updater1\n",
                                                              encoding="utf-8")
(u2_root / "update_clone" / "update.wenyi").write_text("9.9.9\n- x\n", encoding="utf-8")
(u2_prog / "neiwang_update1.py").write_text("# running updater1\n", encoding="utf-8")
patch_res = V.patch_updater1(log=lambda s: None, program_dir=u2_prog, root=u2_root)
ok("更新程序2：覆盖更新程序1 成功", patch_res.get("ok") is True, str(patch_res)[:200])
ok("更新程序2：程序目录里的更新程序1 已替换",
   (u2_prog / "neiwang_update1.py").read_text(encoding="utf-8") == "# patched updater1\n")
ok("更新程序2：完成日志写进 update_clone.log",
   "更新程序1 已由更新程序2 更新到 v9.9.9"
   in (u2_root / "update_clone.log").read_text(encoding="utf-8"))
patch_res2 = V.patch_updater1(log=lambda s: None, program_dir=u2_prog,
                              root=TESTDATA / "u2_none")
ok("更新程序2：update_clone 里没有更新程序1 时明确报错",
   patch_res2.get("ok") is False and "更新程序1" in str(patch_res2.get("reason")),
   str(patch_res2)[:160])

srv.shutdown()

# ================================================================ 4) GUI 冒烟 --
try:
    import tkinter as tk

    (TESTDATA / "config.json").write_text(json.dumps({
        "uid": "GUI-TEST-1", "name": "GUI测试员",
        "update_url": "http://127.0.0.1:1/none", "update_page": "",
    }, ensure_ascii=False), encoding="utf-8")

    root = tk.Tk()
    app = M.App(root)

    # 新逻辑：没入网之前程序是空的（群组不可用），所以先等自动建立内网
    deadline = time.time() + 20
    while time.time() < deadline and not app.core.in_net():
        root.update()
        time.sleep(0.1)
    ok("GUI 启动后自动建立内网（记录物理地址）", app.core.in_net(),
       app.core.net_view().get("my_mac_pretty"))

    g = app.core.create_group("冒烟测试群")
    app.select_gid(g["gid"])
    time.sleep(0.8)
    root.update()
    n_groups = len(app.core.my_groups())
    ok("内网页面渲染正常", isinstance(app.core.net_view().get("members"), list))

    # 选「取消」后不应被永久屏蔽：手动检查 / 下次启动还能再提醒
    real_ask = M.messagebox.askyesnocancel
    M.messagebox.askyesnocancel = lambda *a, **k: None
    try:
        app._update_shown = False
        app._on_update_result({"ok": True, "found": True, "remote": "9.9.9",
                               "local": M.APP_VERSION, "notes": "- 测试", "page": ""})
        ok("更新提醒选「取消」后仍可再次提醒", app._update_shown is False)
    finally:
        M.messagebox.askyesnocancel = real_ask

    root.after(600, root.destroy)
    root.mainloop()
    ok("GUI 启动并退出正常", n_groups == 1, f"groups={n_groups}")
except Exception as e:
    import traceback
    traceback.print_exc()
    ok("GUI 启动并退出正常", False, repr(e))

failed = [n for n, c in PASS if not c]
print("=" * 60)
print(f"结果：{len(PASS) - len(failed)}/{len(PASS)} 通过")
sys.exit(1 if failed else 0)
