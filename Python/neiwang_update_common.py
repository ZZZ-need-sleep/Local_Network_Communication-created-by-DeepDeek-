# -*- coding: utf-8 -*-
"""
内部网 局域网群组 —— 双更新程序公共逻辑（design_v2 第 4 节）
===============================================
本文件是两个独立更新程序的**公共实现**，本身不是入口：

    neiwang_update1.py   更新程序1：备份会话内容 -> 取回新版本 -> 回填 -> **就地更新全部**
                         （除 data/、customer_god/、update_clone/ 与更新程序1 自己以外）
                         完成后带 --wait-pid 调起更新程序2 来更新自己
    neiwang_update2.py   更新程序2：只做一件事 —— 把 update_clone/neiwang_update1.*
                         覆盖到程序目录（覆盖正在运行的更新程序1）

两个入口都可以单独双击运行，命令行参数完全一致（见 design_v2 第 4.2 节）：
    （无）            手动模式：一定给出明确反馈（已是最新 / 无法连接 / 发现新版本并询问）
    --auto           静默：只有发现新版本才弹窗
    --update         不询问，直接开始更新
    --check          无界面，只打印一行：UPDATE_AVAILABLE <ver> / UP_TO_DATE <ver> / OFFLINE
    --patch-updater1 只属于更新程序2：不检查、不下载，直接把 update_clone/ 里的
                     更新程序1 覆盖到程序目录
    --wait-pid <pid> 等待指定进程退出（更新程序1 用它把 pid 交给更新程序2）

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
import time
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
MAIN_PROGRAM_NAME = "neiwang.py"     # 校验取回的新版本是否为完整程序
UPDATE_MANIFEST_NAME = "update.wenyi"
VER_DIR_NAME = "Python"              # 单仓库（monorepo）里本版所在的目录名

UPDATER1_FILE = "neiwang_update1.py"     # 更新程序1（就地更新全部）
UPDATER2_FILE = "neiwang_update2.py"     # 更新程序2（只更新更新程序1）
UPDATER_PID_FILE = "_neiwang_update1.pid"   # 更新程序1 的 pid（供更新程序2 等待）
APPLIED_MARKER = ".update_manifest_applied"

CUSTOMER_DATA_DIR = "customer_god"   # 会话内容备份目录
UPDATE_CLONE_DIR = "update_clone"    # 新版本目录
ZIP_NAME = "update_package.zip"      # 下载下来的 ZIP 更新包
UNZIP_DIR = "_update_unzip"          # 解压临时目录

# 就地更新时**不覆盖 / 不删除**的目录（运行时目录 + 本机数据 + 另一个版本目录）
INPLACE_SKIP_DIRS = {"data", "customer_god", "update_clone", UNZIP_DIR, "__pycache__", "C++"}
# 就地更新时**不覆盖**的文件（更新程序1 自己留给更新程序2）
INPLACE_SKIP_PREFIX = (UPDATER1_FILE[:-3] + ".",)   # "neiwang_update1."
# 「程序文件」白名单：只删除这些类型里、update_clone/ 中已不存在的旧文件
PROGRAM_FILE_SUFFIXES = (".py", ".exe", ".bat")
PROGRAM_FILE_NAMES = {"update.wenyi", "README.md"}
PROGRAM_MD_EXCLUDE_DIRS = {"docs"}

PROGRAM_DIR = Path(__file__).resolve().parent

GITHUB_OWNER = "ZZZ-need-sleep"
GITHUB_REPO = "Local_Network_Communication-created-by-DeepDeek-"
GITHUB_BRANCH = "main"
DEFAULT_UPDATE_URL = (f"https://raw.githubusercontent.com/{GITHUB_OWNER}/{GITHUB_REPO}"
                      f"/{GITHUB_BRANCH}/{VER_DIR_NAME}/{UPDATE_MANIFEST_NAME}")
DEFAULT_UPDATE_REPO = f"https://github.com/{GITHUB_OWNER}/{GITHUB_REPO}.git"
DESKTOP_UPDATE_PAGE = f"https://github.com/{GITHUB_OWNER}/{GITHUB_REPO}"

CHECK_TIMEOUT = 8          # 清单请求超时（秒）
DOWNLOAD_TIMEOUT = 300     # ZIP 更新包下载超时（秒）
GIT_TIMEOUT = 900          # git clone 超时（秒）
WAIT_PID_TIMEOUT = 30.0    # 更新程序2 等待更新程序1 退出的超时（秒）


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


def apply_root_arg(argv) -> str:
    """支持 `--root <path>`（与 C++ 版参数保持一致）。

    等价于设置环境变量 NEIWANG_UPDATE_ROOT；因为 update_root() 是惰性读取的，
    在 main() 最开始调用即可生效。返回解析到的路径（没有则返回空串）。
    """
    argv = list(argv or [])
    for i, a in enumerate(argv):
        s = str(a).strip()
        if s == "--root" and i + 1 < len(argv):
            val = str(argv[i + 1]).strip()
            if val:
                os.environ["NEIWANG_UPDATE_ROOT"] = val
                return val
        elif s.startswith("--root="):
            val = s.split("=", 1)[1].strip()
            if val:
                os.environ["NEIWANG_UPDATE_ROOT"] = val
                return val
    return ""


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
# 进程等待（更新程序2 等更新程序1 退出）
# ----------------------------------------------------------------------------
def pid_alive(pid: int) -> bool:
    """判断进程是否还活着（跨平台，纯标准库）。"""
    try:
        pid = int(pid)
    except Exception:
        return False
    if pid <= 0:
        return False
    if os.name == "nt":
        try:
            import ctypes
            k32 = ctypes.windll.kernel32
            k32.OpenProcess.restype = ctypes.c_void_p
            PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
            h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
            if not h:
                return False
            try:
                code = ctypes.c_ulong(0)
                if not k32.GetExitCodeProcess(ctypes.c_void_p(h), ctypes.byref(code)):
                    return False
                return int(code.value) == 259   # STILL_ACTIVE
            finally:
                k32.CloseHandle(ctypes.c_void_p(h))
        except Exception:
            return True   # 判断不了就当它还活着，交回给上层用重命名兜底
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def wait_for_pid(pid: int, timeout: float = WAIT_PID_TIMEOUT, log=print) -> bool:
    """等待进程退出；超时返回 False。"""
    end = time.time() + max(0.0, float(timeout))
    while time.time() < end:
        if not pid_alive(pid):
            return True
        time.sleep(0.2)
    return not pid_alive(pid)


def write_pid_file(root: Path, pid: int = None):
    try:
        (Path(root) / UPDATER_PID_FILE).write_text(str(int(pid or os.getpid())),
                                                   encoding="utf-8")
    except Exception:
        pass


def read_pid_file(root: Path):
    try:
        return int((Path(root) / UPDATER_PID_FILE).read_text(encoding="utf-8").strip())
    except Exception:
        return None


def append_clone_log(root: Path, text: str):
    """更新日志 update_clone.log 追加一行。"""
    try:
        with open(Path(root) / "update_clone.log", "a", encoding="utf-8") as f:
            f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {text}\n")
    except Exception:
        pass


# ----------------------------------------------------------------------------
# 更新动作：备份 -> 取回新版本 -> 回填
# ----------------------------------------------------------------------------
def backup_customer_data(data_root: Path, backup: Path, log=print):
    """把用户所有会话内容备份到 customer_god/（先清空）。"""
    data_root = Path(data_root)
    data_root.mkdir(parents=True, exist_ok=True)
    backup = Path(backup)
    if backup.exists():
        shutil.rmtree(backup, ignore_errors=True)
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
        with open(logfile, "a", encoding="utf-8", errors="replace") as f:
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


def flatten_version_dir(target: Path) -> bool:
    """单仓库（monorepo）兼容：仓库根下有 Python/ 与 C++/ 两个版本目录。

    git clone / ZIP 解压后拿到的可能是「仓库根」或「版本目录」。
    若拿到的是仓库根，则**只保留本版目录的内容**（其它版本目录会被丢掉），
    保证 Python 版更新时不会把 C++/ 目录塞进自己的安装目录。
    返回 target 下是否已存在本版主程序。
    """
    target = Path(target)
    if (target / MAIN_PROGRAM_NAME).is_file():
        return True
    ver = target / VER_DIR_NAME
    if not (ver / MAIN_PROGRAM_NAME).is_file():
        return False
    stage = target.parent / (target.name + "__ver")
    shutil.rmtree(stage, ignore_errors=True)
    shutil.move(str(ver), str(stage))
    shutil.rmtree(target, ignore_errors=True)
    shutil.move(str(stage), str(target))
    return (target / MAIN_PROGRAM_NAME).is_file()


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
            if ok and flatten_version_dir(target):
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
            if flatten_version_dir(target):
                return {"ok": True, "method": "zip"}
            tried.append(f"ZIP 更新包里没有找到 {MAIN_PROGRAM_NAME}（包内容不完整）")
        except Exception as e:
            tried.append(f"ZIP 更新包失败：{type(e).__name__}: {e}")
        finally:
            shutil.rmtree(unzip_dir, ignore_errors=True)
    else:
        tried.append("没有配置 update_package（ZIP 更新包地址）")

    return {"ok": False, "reason": "取回新版本失败：\n- " + "\n- ".join(tried)}


# ----------------------------------------------------------------------------
# 第四步：就地更新全部（更新程序1）
# ----------------------------------------------------------------------------
def is_program_file(rel: Path) -> bool:
    """「程序文件」白名单：*.py / *.exe / *.bat / update.wenyi / README.md / *.md（docs/ 除外）。"""
    rel = Path(rel)
    name = rel.name
    if name in PROGRAM_FILE_NAMES:
        return True
    if rel.suffix.lower() in PROGRAM_FILE_SUFFIXES:
        return True
    if rel.suffix.lower() == ".md":
        first = rel.parts[0] if rel.parts else ""
        return first not in PROGRAM_MD_EXCLUDE_DIRS
    return False


def _inplace_skip(rel: Path) -> bool:
    rel = Path(rel)
    parts = rel.parts
    if parts and parts[0] in INPLACE_SKIP_DIRS:
        return True
    if str(rel.name).startswith(INPLACE_SKIP_PREFIX):
        return True
    return False


def inplace_update(clone: Path, root: Path, log=print) -> dict:
    """第四步：从 update_clone/ 把除 data/、customer_god/、update_clone/ 与
    更新程序1 自己以外的全部文件覆盖到程序目录（保持相对路径），
    并删除 update_clone/ 中不存在、但属于「程序文件」的旧文件。"""
    clone = Path(clone)
    root = Path(root)
    if not clone.is_dir():
        return {"ok": False, "reason": f"新版本目录不存在：{clone}"}
    copied = 0
    for src in sorted(clone.rglob("*")):
        if not src.is_file():
            continue
        rel = src.relative_to(clone)
        if _inplace_skip(rel):
            continue
        dst = root / rel
        try:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            copied += 1
        except Exception as e:
            return {"ok": False, "reason": f"覆盖 {rel} 失败：{type(e).__name__}: {e}"}
    removed = 0
    for dst in sorted(root.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        if not dst.is_file():
            continue
        rel = dst.relative_to(root)
        if _inplace_skip(rel):
            continue
        if (clone / rel).exists():
            continue
        if not is_program_file(rel):
            continue
        try:
            dst.unlink()
            removed += 1
            log(f"    已删除旧文件：{rel}")
        except Exception:
            pass
    return {"ok": True, "copied": copied, "removed": removed}


def write_applied_marker(root: Path, version: str):
    try:
        (Path(root) / APPLIED_MARKER).write_text(
            json.dumps({"version": str(version), "ts": time.time(),
                        "time": time.strftime("%Y-%m-%d %H:%M:%S")},
                       ensure_ascii=False, indent=1), encoding="utf-8")
    except Exception:
        pass


def find_updater(root: Path, clone: Path, name: str):
    """优先 update_clone/ 里的新版，其次程序目录里的。"""
    for base in (Path(clone), Path(root)):
        p = Path(base) / name
        if p.is_file():
            return p
    return None


def launch_updater2(root: Path, clone: Path, log=print, wait_pid: bool = True) -> bool:
    """第五步：带 --wait-pid 调起更新程序2，让它更新更新程序1 自己。"""
    exe = find_updater(root, clone, UPDATER2_FILE)
    if not exe:
        log(f"没有找到更新程序2（{UPDATER2_FILE}），跳过「更新程序1 更新自己」这一步")
        return False
    write_pid_file(root, os.getpid())
    cmd = [sys.executable or "python", str(exe), "--patch-updater1"]
    if wait_pid:
        cmd += ["--wait-pid", str(os.getpid())]
    try:
        subprocess.Popen(cmd, cwd=str(Path(root)))
        log(f"已调起更新程序2 {exe.name}（--patch-updater1 --wait-pid {os.getpid()}）"
            f"更新更新程序1")
        return True
    except Exception as e:
        log(f"调起更新程序2 失败：{type(e).__name__}: {e}")
        return False


def patch_updater1(log=print, wait_pid=None, program_dir=None, root=None) -> dict:
    """更新程序2 的唯一职责：把 update_clone/neiwang_update1.* 覆盖到程序目录。"""
    root = Path(root) if root else update_root()
    prog = Path(program_dir) if program_dir else PROGRAM_DIR
    clone = root / UPDATE_CLONE_DIR
    pid = wait_pid if wait_pid is not None else read_pid_file(root)
    if pid:
        log(f"等待更新程序1（pid {pid}）退出（最多 {int(WAIT_PID_TIMEOUT)} 秒）...")
        if wait_for_pid(int(pid), WAIT_PID_TIMEOUT, log):
            log("更新程序1 已退出")
        else:
            log("等待超时，仍尝试覆盖（失败则写出 .new 文件）")
    sources = [p for p in sorted(clone.glob(UPDATER1_FILE[:-3] + ".*")) if p.is_file()] \
        if clone.is_dir() else []
    if not sources:
        return {"ok": False,
                "reason": f"{clone} 里没有找到 {UPDATER1_FILE[:-3]}.*（先运行更新程序1）"}
    copied = []
    for src in sources:
        dst = prog / src.name
        try:
            shutil.copy2(src, dst)
            copied.append(dst.name)
        except Exception as e:
            newname = prog / (Path(src.name).stem + ".new" + Path(src.name).suffix)
            try:
                shutil.copy2(src, newname)
            except Exception:
                pass
            return {"ok": False,
                    "reason": (f"覆盖 {dst.name} 失败（{type(e).__name__}: {e}）；"
                               f"已写出 {newname.name}，请关闭本程序后手动改名重启")}
    ver = ""
    try:
        ver = parse_update_manifest(
            (clone / UPDATE_MANIFEST_NAME).read_text(encoding="utf-8")).get("version") or ""
    except Exception:
        ver = ""
    if not ver:
        ver = local_version()
    text = f"更新程序1 已由更新程序2 更新到 v{ver}"
    log(text)
    append_clone_log(root, text)
    return {"ok": True, "files": copied, "version": ver}


def perform_update(cfg: dict, log=print, inplace: bool = False,
                   launch_patch: bool = None) -> dict:
    """完整更新流程：备份会话内容 -> 取回新版本 -> 回填会话内容（-> 就地更新全部）。

    inplace=True 时执行第四步（就地更新全部）与第五步（调起更新程序2 更新自己）。
    返回 {"ok": bool, "path": str, "backup": str, "method": str, "reason": str}
    """
    root = update_root()
    data_root = get_data_dir()
    backup = root / CUSTOMER_DATA_DIR
    target = root / UPDATE_CLONE_DIR
    if launch_patch is None:
        launch_patch = bool(inplace)

    # 1) 备份「用户所有会话内容」
    log(f"1/4 正在把会话内容备份到 {CUSTOMER_DATA_DIR}/ ...")
    try:
        backup_customer_data(data_root, backup, log)
    except Exception as e:
        return {"ok": False, "reason": f"备份会话内容失败：{type(e).__name__}: {e}"}
    log(f"    会话内容已备份：{backup}")

    # 2) 取回新版本
    log("2/4 正在取回新版本（Git 或 ZIP 更新包）...")
    res = fetch_new_version(cfg, target, log)
    if not res.get("ok"):
        return {"ok": False, "reason": res.get("reason", "取回新版本失败")}
    log(f"    新版本已就绪：{target}（方式：{'Git' if res.get('method') == 'git' else 'ZIP 更新包'}）")

    # 3) 会话内容回填到新版本
    log("3/4 正在把会话内容回填到新版本 ...")
    try:
        restore_customer_data(backup, target, log)
    except Exception as e:
        return {"ok": False, "path": str(target), "backup": str(backup),
                "reason": f"回填会话内容失败：{type(e).__name__}: {e}"}

    if not inplace:
        log(f"更新完成，新版本在：{target}")
        return {"ok": True, "path": str(target), "backup": str(backup),
                "method": res.get("method", "")}

    # 4) 就地更新全部（除 data/、customer_god/、update_clone/ 与更新程序1 自己以外）
    log("4/4 正在就地更新程序文件（跳过 data/ 等运行时目录）...")
    ver = ""
    try:
        ver = parse_update_manifest((target / UPDATE_MANIFEST_NAME).read_text(
            encoding="utf-8")).get("version") or ""
    except Exception:
        ver = ""
    out = inplace_update(target, root, log)
    if not out.get("ok"):
        return {"ok": False, "path": str(target), "backup": str(backup),
                "reason": out.get("reason", "就地更新失败")}
    log(f"    就地更新完成：覆盖 {out.get('copied', 0)} 个文件，"
        f"删除 {out.get('removed', 0)} 个旧文件")
    write_applied_marker(root, ver)
    append_clone_log(root, f"更新程序1 已就地更新到 v{ver}（覆盖 {out.get('copied', 0)}，"
                           f"删除 {out.get('removed', 0)}）")

    # 5) 调起更新程序2 更新更新程序1 自己
    patched = False
    if launch_patch:
        patched = launch_updater2(root, target, log)
    log(f"更新完成，新版本在：{target}")
    return {"ok": True, "path": str(target), "backup": str(backup),
            "method": res.get("method", ""), "inplace": out, "version": ver,
            "updater2": patched}


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


def launch_updater1(root: Path = None, log=print) -> bool:
    """调起更新程序1（更新程序2 在普通模式下把真正的更新交给它）。"""
    root = Path(root) if root else update_root()
    exe = find_updater(root, root / UPDATE_CLONE_DIR, UPDATER1_FILE)
    if not exe:
        log(f"没有找到更新程序1（{UPDATER1_FILE}）")
        return False
    try:
        subprocess.Popen([sys.executable or "python", str(exe), "--update"],
                         cwd=str(exe.parent))
        log(f"已启动更新程序1：{exe}")
        return True
    except Exception as e:
        log(f"启动更新程序1 失败：{type(e).__name__}: {e}")
        return False


# ----------------------------------------------------------------------------
# 图形界面（两个更新程序共用；ROLE=1 就地更新全部，ROLE=2 只负责更新更新程序1）
# ----------------------------------------------------------------------------
if TK_AVAILABLE:

    class UpdaterApp(tk.Tk):
        """独立更新程序窗口。

        mode = "manual"（手动，一定给反馈） / "auto"（静默，仅新版本才弹窗） /
               "update"（直接开始更新）
        ROLE = 1（更新程序1）/ 2（更新程序2）
        """

        ROLE = 1
        UPDATER_FILE = UPDATER1_FILE

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

            title = (f"{APP_NAME} 更新程序1" if self.ROLE == 1
                     else f"{APP_NAME} 更新程序2（更新更新程序1 自己）")
            self.title(title)
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

            tk.Label(body, text=(f"{APP_NAME} 更新程序1" if self.ROLE == 1
                                 else f"{APP_NAME} 更新程序2"), bg=self.BG, fg=self.TEXT,
                     font=("Microsoft YaHei UI", 14, "bold")).pack(anchor="w")
            if self.ROLE == 2:
                tk.Label(body, text="更新程序2 只负责把新版的更新程序1 覆盖到程序目录；"
                                    "真正的更新由更新程序1 完成。",
                         bg=self.BG, fg=self.SUB, font=("Microsoft YaHei UI", 9),
                         wraplength=500, justify="left").pack(anchor="w")

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
                if self.ROLE == 1:
                    self._log("点「立即更新」开始：先备份会话内容，再取回新版本，"
                              "把会话内容回填，然后就地更新全部程序文件；")
                    self._log("更新程序1 完成后会自动调起更新程序2 更新它自己。")
                else:
                    self._log("点「立即更新」将启动更新程序1 完成真正的更新。")
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
            if self.ROLE != 1:
                # 更新程序2：把真正的更新交给更新程序1
                if launch_updater1(update_root(), self._log):
                    self._set_status("已启动更新程序1")
                else:
                    messagebox.showwarning(
                        "找不到更新程序1",
                        f"程序目录里没有 {UPDATER1_FILE}，请运行「更新.bat」。", parent=self)
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
            res = perform_update(self.cfg, log=lambda s: self._post(lambda: self._log(s)),
                                 inplace=(self.ROLE == 1))
            self._post(lambda: self._on_update_done(res))

        def _on_update_done(self, res: dict):
            self._busy = False
            if res.get("ok"):
                self._done = True
                self._new_dir = res.get("path")
                self._set_status("更新完成")
                self._log(f"新版本目录：{res.get('path')}")
                self._log(f"会话内容备份：{res.get('backup')}")
                self.btn_folder.configure(state="normal")
                self.btn_launch.configure(state="normal")
                self.btn_close.configure(text="关闭")
                tail = ""
                if res.get("updater2"):
                    tail = (f"\n\n已调起更新程序2（{UPDATER2_FILE}）更新更新程序1 自己，"
                            f"本窗口会在几秒后自动关闭。")
                elif res.get("inplace"):
                    tail = (f"\n\n没能调起更新程序2（{UPDATER2_FILE}），"
                            f"更新程序1 自己需要下次更新时才会替换。")
                messagebox.showinfo(
                    "更新完成",
                    f"已就地更新到 v{res.get('version') or '?'}：\n{res.get('path')}\n\n"
                    f"你的所有会话内容（群组、聊天、文件）已备份到 "
                    f"{CUSTOMER_DATA_DIR}/，并已回填进新版本的 data/ 目录。\n\n"
                    f"请关闭主程序，然后重新双击「启动.bat」使用新版本。" + tail,
                    parent=self)
                if res.get("updater2"):
                    # 尽快退出，让更新程序2 能覆盖更新程序1 自己
                    self.after(4000, self.destroy)
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


def console_mode(cfg: dict, auto: bool, role: int = 1) -> int:
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
    if role != 1:
        print(f"更新程序2 只负责更新更新程序1：请运行 {UPDATER1_FILE} 完成更新。")
        return 0
    if auto:
        print(f"可以用带界面的方式运行本程序来更新：python {UPDATER1_FILE}")
        return 0
    try:
        answer = input("是否现在更新？[y/N] ").strip().lower()
    except Exception:
        answer = "n"
    if answer not in ("y", "yes"):
        print("已取消。")
        return 0
    out = perform_update(cfg, log=print, inplace=True)
    print("更新完成：" + str(out.get("path")) if out.get("ok")
          else "更新失败：" + str(out.get("reason")))
    return 0 if out.get("ok") else 1


def usage(updater_file: str, role: int = 1) -> str:
    name = "更新程序1" if role == 1 else "更新程序2（只更新更新程序1）"
    extra = ("" if role == 1 else
             f"  python {updater_file} --patch-updater1 [--wait-pid <pid>]\n"
             f"                                  把 update_clone/ 里的更新程序1 覆盖到程序目录\n")
    return (f"{APP_NAME} {name}（独立程序，可单独运行）\n"
            f"用法：\n"
            f"  python {updater_file}           检查并更新（有界面）\n"
            f"  python {updater_file} --auto    静默检查：无网络/版本相同不打扰，有新版本才弹窗\n"
            f"  python {updater_file} --update  直接开始更新\n"
            f"  python {updater_file} --check   无界面检查，打印一行结果\n"
            + extra)
