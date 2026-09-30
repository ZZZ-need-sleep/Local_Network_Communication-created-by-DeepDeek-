# -*- coding: utf-8 -*-
"""
内部网 局域网群组 —— 独立更新程序
===============================================
这是**单独的一个程序**：可以脱离主程序单独运行，主程序 neiwang.py 里没有更新代码，
它只负责「启动时静默检查 + 发现新版本时弹窗提醒」，真正的更新动作全部在这里完成。

用法：
    python neiwang_update.py            # 手动模式：有界面，任何情况都给出明确反馈
    python neiwang_update.py --auto     # 自动模式：无网络 / 版本号相同或更旧 -> 静默退出；
                                        #           只有发现新版本才弹窗
    python neiwang_update.py --update    # 不再询问，直接开始更新（主程序点「是」后就是这样调起它的）
    python neiwang_update.py --check     # 无界面检查：只打印一行结果，便于脚本/内网批量排查

更新三步（全部自动）：
    1) 把 data/（用户所有会话内容：群组、聊天、文件索引、转存记录）备份到 customer_god/；
    2) 把新版本取到 update_clone/：
         - 装了 Git 且配置了 update_repo  -> git clone --depth 1
         - 否则配置了 update_package      -> 下载 ZIP 更新包并解压（不需要 Git，适合内网部署）
    3) 把 customer_god/ 里的会话内容回填到新版本的 data/ 目录。

零第三方依赖，Python 3.8+ 即可。
"""

import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import traceback
import urllib.request
import zipfile
from pathlib import Path

try:
    import tkinter as tk
    from tkinter import messagebox
    TK_AVAILABLE = True
except Exception:          # 没有 tkinter 的环境（极少见）仍可用 --check 或控制台更新
    tk = None
    messagebox = None
    TK_AVAILABLE = False

# ----------------------------------------------------------------------------
# 常量 / 配置（与主程序 neiwang.py 保持一致）
# ----------------------------------------------------------------------------
APP_NAME = "内部网"
UPDATER_FILE = "neiwang_update.py"
MAIN_PROGRAM_NAME = "neiwang.py"     # 校验取回的新版本是否为完整程序
UPDATE_MANIFEST_NAME = "update.wenyi"

CUSTOMER_DATA_DIR = "customer_god"   # 会话内容备份目录
UPDATE_CLONE_DIR = "update_clone"    # 新版本目录
ZIP_NAME = "update_package.zip"      # 下载下来的 ZIP 更新包
UNZIP_DIR = "_update_unzip"          # 解压临时目录

PROGRAM_DIR = Path(__file__).resolve().parent

GITHUB_OWNER = "ZZZ-need-sleep"
GITHUB_REPO = "Local_Network_Communication-created-by-DeepDeek-"
GITHUB_BRANCH = "main"
DEFAULT_UPDATE_URL = (f"https://raw.githubusercontent.com/{GITHUB_OWNER}/{GITHUB_REPO}"
                      f"/{GITHUB_BRANCH}/{UPDATE_MANIFEST_NAME}")
DEFAULT_UPDATE_REPO = f"https://github.com/{GITHUB_OWNER}/{GITHUB_REPO}.git"
DESKTOP_UPDATE_PAGE = f"https://github.com/{GITHUB_OWNER}/{GITHUB_REPO}"

CHECK_TIMEOUT = 8          # 清单请求超时（秒）
DOWNLOAD_TIMEOUT = 300     # ZIP 更新包下载超时（秒）
GIT_TIMEOUT = 900          # git clone 超时（秒）


# ----------------------------------------------------------------------------
# 小工具
# ----------------------------------------------------------------------------
def get_data_dir() -> Path:
    """数据目录（默认程序同目录 data/；测试可用环境变量 NEIWANG_DATA_DIR 覆盖）。"""
    env = os.environ.get("NEIWANG_DATA_DIR")
    base = Path(env) if env else PROGRAM_DIR / "data"
    base.mkdir(parents=True, exist_ok=True)
    return base


def update_root() -> Path:
    """更新工作目录（默认程序目录；测试可用环境变量 NEIWANG_UPDATE_ROOT 覆盖）。"""
    env = os.environ.get("NEIWANG_UPDATE_ROOT")
    return Path(env) if env else PROGRAM_DIR


def parse_update_manifest(data: str) -> dict:
    """解析更新清单，返回 {"version": str, "notes": str}。

    支持两种格式：
      1) JSON：{"version": "1.0.1", "notes": "..."}（notes 可为字符串或字符串数组）
      2) 纯文本：第一行为版本号，其余各行作为「更新内容」
    """
    data = (data or "").strip()
    if not data:
        return {"version": "", "notes": ""}
    try:
        obj = json.loads(data)
        if isinstance(obj, dict) and "version" in obj:
            ver = str(obj.get("version", "")).strip()
            notes = obj.get("notes") or obj.get("content") or obj.get("changelog") or ""
            if isinstance(notes, list):
                notes = "\n".join(str(x) for x in notes)
            else:
                notes = str(notes).strip()
            return {"version": ver, "notes": notes}
    except Exception:
        pass
    lines = data.splitlines()
    ver = lines[0].strip()
    notes = "\n".join(lines[1:]).strip()
    return {"version": ver, "notes": notes}


def version_key(ver: str) -> tuple:
    """把版本号转成可比较的整数元组：'1.1.1' -> (1, 1, 1)。"""
    parts = re.findall(r"\d+", str(ver or ""))
    return tuple(int(x) for x in parts) if parts else (0,)


def is_newer_version(remote: str, local: str) -> bool:
    """只有远程版本「比本地新」才算有更新；版本号相同或更旧都当作无更新。"""
    r, l = version_key(remote), version_key(local)
    n = max(len(r), len(l))
    r = r + (0,) * (n - len(r))
    l = l + (0,) * (n - len(l))
    return r > l


def local_manifest_path() -> Path:
    return PROGRAM_DIR / UPDATE_MANIFEST_NAME


def local_version() -> str:
    """本地版本号 = 程序目录 update.wenyi 的第一行。"""
    try:
        m = parse_update_manifest(local_manifest_path().read_text(encoding="utf-8"))
        return m.get("version") or "0"
    except Exception:
        return "0"


def load_config() -> dict:
    """读取主程序的 data/config.json（读不到就用默认值）。"""
    cfg = {
        "update_url": DEFAULT_UPDATE_URL,
        "update_page": DESKTOP_UPDATE_PAGE,
        "update_repo": DEFAULT_UPDATE_REPO,
        "update_package": "",
    }
    try:
        data = json.loads((get_data_dir() / "config.json").read_text(encoding="utf-8"))
        if isinstance(data, dict):
            cfg.update(data)
    except Exception:
        pass
    return cfg


def have_git() -> bool:
    return shutil.which("git") is not None


def fetch_manifest(url: str, timeout: float = CHECK_TIMEOUT) -> dict:
    """请求远程 update.wenyi；失败（没联网等）返回 None。"""
    try:
        req = urllib.request.Request(
            url, headers={"User-Agent": f"NeiWangUpdater/{local_version()}"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = r.read(65536).decode("utf-8", "replace")
        return parse_update_manifest(data)
    except Exception:
        return None


def check_for_update(cfg: dict) -> dict:
    """检查更新，返回 {"status": "update"|"current"|"offline", ...}。

    - "offline"：没配置地址、没联网、服务器无响应；
    - "current"：远程版本号与本地相同或更旧；
    - "update" ：远程版本更新（此时才需要提醒用户）。
    """
    local = local_version()
    url = str(cfg.get("update_url") or "").strip()
    if not url:
        return {"status": "offline", "local": local, "reason": "没有配置 update_url（更新清单地址）"}
    m = fetch_manifest(url)
    if m is None:
        return {"status": "offline", "local": local,
                "reason": f"无法连接更新服务器：{url}"}
    remote = str(m.get("version") or "").strip()
    if remote and is_newer_version(remote, local):
        return {"status": "update", "local": local, "remote": remote,
                "notes": m.get("notes", "")}
    return {"status": "current", "local": local, "remote": remote}


# ----------------------------------------------------------------------------
# 更新动作：备份 -> 取回新版本 -> 回填
# ----------------------------------------------------------------------------
def backup_customer_data(data_root: Path, backup: Path, log=print):
    """把用户所有会话内容备份到 customer_god/。"""
    data_root = Path(data_root)
    data_root.mkdir(parents=True, exist_ok=True)
    backup = Path(backup)
    backup.mkdir(parents=True, exist_ok=True)
    shutil.copytree(data_root, backup, dirs_exist_ok=True)


def restore_customer_data(backup: Path, target: Path, log=print):
    """把 customer_god/ 里备份的会话内容回填到新版本目录的 data/。"""
    newdata = Path(target) / "data"
    newdata.mkdir(parents=True, exist_ok=True)
    for item in Path(backup).iterdir():
        dst = newdata / item.name
        if item.is_dir():
            shutil.copytree(item, dst, dirs_exist_ok=True)
        else:
            shutil.copy2(item, dst)


def download_file(url: str, dest: Path, log=print, timeout: float = DOWNLOAD_TIMEOUT):
    """下载文件到 dest（纯标准库，带粗略进度提示）。"""
    req = urllib.request.Request(
        url, headers={"User-Agent": f"NeiWangUpdater/{local_version()}"})
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    log(f"    正在下载：{url}")
    last_mark = -1
    got = 0
    with urllib.request.urlopen(req, timeout=timeout) as r, open(dest, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        while True:
            chunk = r.read(64 * 1024)
            if not chunk:
                break
            f.write(chunk)
            got += len(chunk)
            mark = got // (2 * 1024 * 1024)      # 每 2MB 提示一次
            if mark != last_mark:
                last_mark = mark
                if total:
                    log(f"    已下载 {got // 1024} KB / {total // 1024} KB")
                else:
                    log(f"    已下载 {got // 1024} KB")
    log(f"    下载完成：{dest}（{got // 1024} KB）")


def safe_extract(zf: zipfile.ZipFile, dest: Path):
    """解压 ZIP，并拒绝绝对路径 / 含 .. 的危险成员（防目录穿越）。"""
    dest = Path(dest).resolve()
    for member in zf.infolist():
        name = str(member.filename).replace("\\", "/")
        if name.startswith("/") or re.match(r"^[A-Za-z]:", name) or ".." in name.split("/"):
            raise ValueError(f"压缩包里含有不安全的路径，已中止：{member.filename}")
        zf.extract(member, dest)


def single_root(folder: Path) -> Path:
    """压缩包内若只有一层根目录（GitHub 的 ZIP 就是这样），返回那一层。"""
    items = list(Path(folder).iterdir())
    if len(items) == 1 and items[0].is_dir():
        return items[0]
    return Path(folder)


def run_git_clone(repo: str, target: Path, log=print):
    """执行 git clone（输出写入 update_clone.log，避免管道问题），返回 (ok, 输出)。"""
    logfile = update_root() / "update_clone.log"
    log(f"    git clone --depth 1 {repo}")
    try:
        with open(logfile, "w", encoding="utf-8", errors="replace") as f:
            proc = subprocess.run(["git", "clone", "--depth", "1", repo, str(target)],
                                  stdout=f, stderr=subprocess.STDOUT, timeout=GIT_TIMEOUT)
    except FileNotFoundError:
        return False, "未找到 git 命令：本机没有安装 Git"
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"
    try:
        out = logfile.read_text(encoding="utf-8", errors="replace")
    except Exception:
        out = ""
    return proc.returncode == 0, out.strip()[-400:]


def fetch_new_version(cfg: dict, target: Path, log=print) -> dict:
    """把新版本取到 target（update_clone/）：先试 Git，再试 ZIP 更新包。

    返回 {"ok": bool, "method": "git"|"zip", "reason": str}
    """
    repo = str(cfg.get("update_repo") or "").strip()
    package = str(cfg.get("update_package") or "").strip()
    target = Path(target)
    tried = []

    if target.exists():
        shutil.rmtree(target, ignore_errors=True)

    # 方式一：git clone（需要本机装了 Git）
    if repo and "<" not in repo:
        if have_git():
            ok, out = run_git_clone(repo, target, log)
            if ok and (target / MAIN_PROGRAM_NAME).is_file():
                return {"ok": True, "method": "git"}
            tried.append("Git 方式失败：" + (out or "仓库内容不完整（没有找到 "
                                             f"{MAIN_PROGRAM_NAME}）"))
            shutil.rmtree(target, ignore_errors=True)
        else:
            tried.append("本机没有安装 Git，无法用 Git 方式更新")
    else:
        tried.append("没有配置 update_repo（Git 仓库地址）")

    # 方式二：下载 ZIP 更新包（不需要 Git，适合内网部署）
    if package and "<" not in package:
        zip_path = update_root() / ZIP_NAME
        unzip_dir = update_root() / UNZIP_DIR
        try:
            download_file(package, zip_path, log)
            shutil.rmtree(unzip_dir, ignore_errors=True)
            unzip_dir.mkdir(parents=True, exist_ok=True)
            log("    正在解压更新包 ...")
            with zipfile.ZipFile(zip_path) as zf:
                safe_extract(zf, unzip_dir)
            src = single_root(unzip_dir)
            target.mkdir(parents=True, exist_ok=True)
            shutil.copytree(src, target, dirs_exist_ok=True)
            if (target / MAIN_PROGRAM_NAME).is_file():
                return {"ok": True, "method": "zip"}
            tried.append(f"ZIP 更新包里没有找到 {MAIN_PROGRAM_NAME}（包内容不完整）")
        except Exception as e:
            tried.append(f"ZIP 更新包失败：{type(e).__name__}: {e}")
        finally:
            shutil.rmtree(unzip_dir, ignore_errors=True)
    else:
        tried.append("没有配置 update_package（ZIP 更新包地址）")

    return {"ok": False, "reason": "取回新版本失败：\n- " + "\n- ".join(tried)}


def perform_update(cfg: dict, log=print) -> dict:
    """完整更新流程：备份会话内容 -> 取回新版本 -> 回填会话内容。

    返回 {"ok": bool, "path": str, "backup": str, "method": str, "reason": str}
    """
    root = update_root()
    data_root = get_data_dir()
    backup = root / CUSTOMER_DATA_DIR
    target = root / UPDATE_CLONE_DIR

    # 1) 备份「用户所有会话内容」
    log(f"1/3 正在把会话内容备份到 {CUSTOMER_DATA_DIR}/ ...")
    try:
        backup_customer_data(data_root, backup, log)
    except Exception as e:
        return {"ok": False, "reason": f"备份会话内容失败：{type(e).__name__}: {e}"}
    log(f"    会话内容已备份：{backup}")

    # 2) 取回新版本
    log("2/3 正在取回新版本（Git 或 ZIP 更新包）...")
    res = fetch_new_version(cfg, target, log)
    if not res.get("ok"):
        return {"ok": False, "reason": res.get("reason", "取回新版本失败")}
    log(f"    新版本已就绪：{target}（方式：{'Git' if res.get('method') == 'git' else 'ZIP 更新包'}）")

    # 3) 会话内容回填到新版本
    log("3/3 正在把会话内容回填到新版本 ...")
    try:
        restore_customer_data(backup, target, log)
    except Exception as e:
        return {"ok": False, "path": str(target), "backup": str(backup),
                "reason": f"回填会话内容失败：{type(e).__name__}: {e}"}
    log(f"更新完成，新版本在：{target}")
    return {"ok": True, "path": str(target), "backup": str(backup),
            "method": res.get("method", "")}


# ----------------------------------------------------------------------------
# 用系统默认方式打开目录 / 启动新版本
# ----------------------------------------------------------------------------
def open_folder(path: Path):
    path = Path(path)
    try:
        if os.name == "nt":
            os.startfile(str(path))                      # noqa: S606
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(path)])
        else:
            subprocess.Popen(["xdg-open", str(path)])
    except Exception:
        pass


def launch_new_version(new_dir: Path):
    """启动新版本：Windows 上优先用 启动.bat（脚本里会优先用 pythonw）。"""
    new_dir = Path(new_dir)
    bat = new_dir / "启动.bat"
    try:
        if os.name == "nt" and bat.is_file():
            os.startfile(str(bat))                       # noqa: S606
            return True
        main_py = new_dir / MAIN_PROGRAM_NAME
        if main_py.is_file():
            subprocess.Popen([sys.executable or "python", str(main_py)], cwd=str(new_dir))
            return True
    except Exception:
        return False
    return False


# ----------------------------------------------------------------------------
# 图形界面
# ----------------------------------------------------------------------------
if TK_AVAILABLE:

    class UpdaterApp(tk.Tk):
        """独立更新程序窗口。

        mode = "manual"（手动，一定给反馈） / "auto"（静默，仅新版本才弹窗） /
               "update"（直接开始更新）
        """

        BG = "#ffffff"
        PANEL = "#f7f7f7"
        TEXT = "#191919"
        SUB = "#9a9a9a"
        ACCENT = "#07c160"
        ACCENT_HV = "#06ad56"
        BORDER = "#e5e5e5"

        def __init__(self, mode: str, cfg: dict):
            super().__init__()
            self.mode = mode
            self.cfg = cfg
            self._busy = False
            self._done = False
            self._new_dir = None
            self._q = queue.Queue()

            self.title(f"{APP_NAME} 更新程序")
            self.geometry("560x460")
            self.minsize(500, 400)
            self.configure(bg=self.BG)
            if mode == "auto":
                self.withdraw()          # 静默模式：先不显示，只有发现新版本才弹出来

            self._build()
            self.after(100, self._poll)
            self.after(150, self._start)

        # ------------------------------------------------------------ 界面 --
        def _build(self):
            body = tk.Frame(self, bg=self.BG, padx=20, pady=16)
            body.pack(fill="both", expand=True)

            tk.Label(body, text=f"{APP_NAME} 更新程序", bg=self.BG, fg=self.TEXT,
                     font=("Microsoft YaHei UI", 14, "bold")).pack(anchor="w")

            self.status_lbl = tk.Label(body, text="正在检查更新 ...", bg=self.BG,
                                       fg=self.TEXT, font=("Microsoft YaHei UI", 11),
                                       wraplength=500, justify="left")
            self.status_lbl.pack(anchor="w", pady=(10, 2))

            self.version_lbl = tk.Label(body, text=f"当前版本 v{local_version()}",
                                        bg=self.BG, fg=self.SUB,
                                        font=("Microsoft YaHei UI", 9))
            self.version_lbl.pack(anchor="w")

            self.notes_txt = tk.Text(body, height=6, bg=self.PANEL, fg=self.TEXT,
                                     relief="flat", bd=0, wrap="word",
                                     font=("Microsoft YaHei UI", 10))
            self.notes_txt.pack(fill="x", pady=(10, 0))
            self.notes_txt.insert("1.0", "（暂时没有更新内容）")
            self.notes_txt.configure(state="disabled")

            tk.Label(body, text="进度", bg=self.BG, fg=self.SUB,
                     font=("Microsoft YaHei UI", 9)).pack(anchor="w", pady=(10, 2))
            self.log_txt = tk.Text(body, height=8, bg=self.PANEL, fg=self.TEXT,
                                   relief="flat", bd=0, wrap="word",
                                   font=("Consolas", 9))
            self.log_txt.pack(fill="both", expand=True)
            self.log_txt.configure(state="disabled")

            row = tk.Frame(body, bg=self.BG)
            row.pack(fill="x", pady=(12, 0))
            self.btn_update = self._button(row, "立即更新", self._begin_update, primary=True)
            self.btn_folder = self._button(row, "打开新版本文件夹", self._open_new_dir)
            self.btn_launch = self._button(row, "启动新版本", self._launch_new)
            self.btn_page = self._button(row, "打开发布页", self._open_page)
            self.btn_close = self._button(row, "关闭", self.destroy)
            self.btn_update.configure(state="disabled")
            self.btn_folder.configure(state="disabled")
            self.btn_launch.configure(state="disabled")
            self.btn_page.configure(state="disabled")

        def _button(self, parent, text, cmd, primary=False):
            bg = self.ACCENT if primary else self.PANEL
            fg = "#ffffff" if primary else self.TEXT
            b = tk.Button(parent, text=text, command=cmd, bg=bg, fg=fg,
                          activebackground=self.ACCENT_HV if primary else self.BORDER,
                          activeforeground=fg, relief="flat", bd=0, cursor="hand2",
                          font=("Microsoft YaHei UI", 9), padx=10, pady=4)
            b.pack(side="left", padx=(0, 6))
            return b

        # ------------------------------------------------------ 线程 -> 界面 --
        def _post(self, fn):
            self._q.put(fn)

        def _poll(self):
            try:
                while True:
                    self._q.get_nowait()()
            except queue.Empty:
                pass
            self.after(100, self._poll)

        def _log(self, text: str):
            self.log_txt.configure(state="normal")
            self.log_txt.insert("end", str(text) + "\n")
            self.log_txt.see("end")
            self.log_txt.configure(state="disabled")

        def _set_status(self, text: str):
            self.status_lbl.configure(text=text)

        def _show_notes(self, notes: str):
            self.notes_txt.configure(state="normal")
            self.notes_txt.delete("1.0", "end")
            self.notes_txt.insert("1.0", notes or "（本次更新没有提供更新内容）")
            self.notes_txt.configure(state="disabled")

        # ------------------------------------------------------------- 检查 --
        def _start(self):
            if self.mode == "update":
                self._begin_update()
                return
            self._set_status("正在检查更新 ...")
            threading.Thread(target=self._check_worker, name="check", daemon=True).start()

        def _check_worker(self):
            res = check_for_update(self.cfg)
            self._post(lambda: self._on_check(res))

        def _on_check(self, res: dict):
            status = res.get("status")
            self.version_lbl.configure(text=f"当前版本 v{res.get('local', local_version())}")

            if status == "update":
                if self.mode == "auto":
                    self.deiconify()          # 只有发现新版本才让窗口出现
                    self.lift()
                self._set_status(f"发现新版本 —— v{res.get('remote')}"
                                 f"（当前 v{res.get('local')}）")
                self._show_notes(res.get("notes", ""))
                self._log(f"远程版本：v{res.get('remote')}")
                self._log("点「立即更新」开始：先备份会话内容，再取回新版本，"
                          "最后把会话内容回填到新版本。")
                self.btn_update.configure(state="normal")
                if self._page():
                    self.btn_page.configure(state="normal")
                return

            if status == "current":
                if self.mode == "auto":
                    self.destroy()            # 版本号相同 -> 静默退出，不打扰用户
                    return
                self._set_status(f"已经是最新版本（v{res.get('local')}）")
                self._log("远程版本与本地相同或更旧，不需要更新。")
                return

            # offline
            if self.mode == "auto":
                self.destroy()                # 没联网 -> 静默退出，不打扰用户
                return
            self._set_status("无法连接更新服务器")
            self._log(res.get("reason", "没有网络或地址不可用"))
            self._log("请检查网络连接，或确认 config.json 里的 update_url 是否正确。")
            if self._page():
                self.btn_page.configure(state="normal")

        def _page(self) -> str:
            return str(self.cfg.get("update_page") or DESKTOP_UPDATE_PAGE).strip()

        def _open_page(self):
            page = self._page()
            if page:
                try:
                    import webbrowser
                    webbrowser.open(page)
                except Exception:
                    pass

        # ------------------------------------------------------------- 更新 --
        def _begin_update(self):
            if self._busy or self._done:
                return
            self._busy = True
            if self.mode == "auto":
                self.deiconify()
                self.lift()
            self.btn_update.configure(state="disabled")
            self.btn_page.configure(state="disabled")
            self._set_status("正在更新 ...")
            self._log("开始更新。")
            threading.Thread(target=self._update_worker, name="update", daemon=True).start()

        def _update_worker(self):
            res = perform_update(self.cfg, log=lambda s: self._post(lambda: self._log(s)))
            self._post(lambda: self._on_update_done(res))

        def _on_update_done(self, res: dict):
            self._busy = False
            if res.get("ok"):
                self._done = True
                self._new_dir = res.get("path")
                self._set_status("更新完成，请到新版本目录使用")
                self._log(f"新版本目录：{res.get('path')}")
                self._log(f"会话内容备份：{res.get('backup')}")
                self.btn_folder.configure(state="normal")
                self.btn_launch.configure(state="normal")
                self.btn_close.configure(text="关闭")
                messagebox.showinfo(
                    "更新完成",
                    f"新版本已就绪：\n{res.get('path')}\n\n"
                    f"你的所有会话内容（群组、聊天、文件）已备份到 "
                    f"{CUSTOMER_DATA_DIR}/，并已回填进新版本的 data/ 目录。\n\n"
                    f"请关闭主程序，然后到上面的目录双击「启动.bat」使用新版本\n"
                    f"（也可以直接点下面的「启动新版本」）。",
                    parent=self)
            else:
                self._set_status("更新失败")
                self._log(str(res.get("reason", "未知错误")))
                self.btn_update.configure(state="normal")
                messagebox.showwarning("更新失败", str(res.get("reason", "未知错误")),
                                       parent=self)

        def _open_new_dir(self):
            if self._new_dir:
                open_folder(self._new_dir)

        def _launch_new(self):
            if self._new_dir:
                if not launch_new_version(self._new_dir):
                    messagebox.showwarning(
                        "启动失败",
                        f"没能自动启动，请手动到下面目录双击「启动.bat」：\n{self._new_dir}",
                        parent=self)


# ----------------------------------------------------------------------------
# 无界面：--check 与控制台回退
# ----------------------------------------------------------------------------
def cmd_check(cfg: dict) -> int:
    """无界面检查：只打印一行结果。"""
    res = check_for_update(cfg)
    status = res.get("status")
    if status == "update":
        print(f"UPDATE_AVAILABLE {res.get('remote')}")
    elif status == "current":
        print(f"UP_TO_DATE {res.get('local')}")
    else:
        print("OFFLINE")
    return 0


def console_mode(cfg: dict, auto: bool) -> int:
    """没有 tkinter 时的控制台模式（--auto 时静默退出）。"""
    res = check_for_update(cfg)
    status = res.get("status")
    if status == "current":
        if not auto:
            print(f"已经是最新版本（v{res.get('local')}）。")
        return 0
    if status == "offline":
        if not auto:
            print("无法连接更新服务器：" + str(res.get("reason", "")))
        return 0
    print(f"发现新版本 —— v{res.get('remote')}（当前 v{res.get('local')}）")
    if res.get("notes"):
        print("更新内容：")
        print(res["notes"])
    if auto:
        print("可以用带界面的方式运行本程序来更新：python neiwang_update.py")
        return 0
    try:
        answer = input("是否现在更新？[y/N] ").strip().lower()
    except Exception:
        answer = "n"
    if answer not in ("y", "yes"):
        print("已取消。")
        return 0
    out = perform_update(cfg, log=print)
    print("更新完成：" + str(out.get("path")) if out.get("ok")
          else "更新失败：" + str(out.get("reason")))
    return 0 if out.get("ok") else 1


def usage() -> str:
    return (f"{APP_NAME} 更新程序（独立程序，可单独运行）\n"
            f"用法：\n"
            f"  python {UPDATER_FILE}           检查并更新（有界面）\n"
            f"  python {UPDATER_FILE} --auto    静默检查：无网络/版本相同不打扰，有新版本才弹窗\n"
            f"  python {UPDATER_FILE} --update  直接开始更新\n"
            f"  python {UPDATER_FILE} --check   无界面检查，打印一行结果\n")


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    flags = {a.strip().lower() for a in argv}

    if flags & {"-h", "--help", "/?"}:
        print(usage())
        return 0

    cfg = load_config()

    if flags & {"-c", "--check"}:
        return cmd_check(cfg)

    if flags & {"-a", "--auto"}:
        mode = "auto"
    elif flags & {"-u", "--update"}:
        mode = "update"
    else:
        mode = "manual"

    if not TK_AVAILABLE:
        return console_mode(cfg, auto=(mode == "auto"))

    try:
        app = UpdaterApp(mode, cfg)
    except Exception:
        traceback.print_exc()
        if mode == "auto":
            return 0
        return console_mode(cfg, auto=False)

    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
