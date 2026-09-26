# -*- coding: utf-8 -*-
"""
MCTier 局域网群组（中文简称：班群助手）
===============================================
纯 Python 标准库实现的局域网（校园网/班级局域网）群组聊天 + 群文件共享软件。

功能对照需求：
  1. 发送文字（群组聊天、广播，同组成员可见）
  2. 双人私聊：点成员头像快速私聊；对方离线时自动加密转存到某位在线同学，
     待对方上线自动送达，送达后校验完整性并删除转存（转存方全程无感、无法查看）
  3. 文件功能：向群组发送文件；检测到已加入群组的新文件 -> 自动下载
  4. 联网检查更新：每次启动自动检查；未联网 / 无更新 -> 无任何反馈；
     仅当联网成功且版本号不同 -> 弹窗提醒用户更新
  5. 局域网内设置群组（班级 / 小组），可加入多个群组，也可退出某个群组
  6. 每个用户在群组内显示自己的名字：第一次改名自由，之后的修改需超过 5 名用户
     投票同意（其他电脑收到投票弹窗，同意票足够后名字自动更新）
  7. 文件分发策略（按你的要求）：
       - 校验发送人是否存在（在线）-> 存在则向发送人点对点下载
       - 发送人不在线 -> 在「日志」里公共询问（全网可见，所有人可看不可操作），
         其他下载过该文件的用户返回自己的局域网 IP（没下载过的无应答），
         向第一个返回的 IP 自动请求文件（全程无需两端用户操作）
  8. 下载完成后调用系统 API 弹通知：「qqq」群「AA」的「你好」下载完毕
  9. 所有文件打开时自动用系统默认应用程序打开

运行：python mctier_lan.py    （Windows 可双击 启动.bat）
零第三方依赖，Python 3.8+ 即可。
"""

import ctypes
import hashlib
import hmac
import json
import os
import queue
import random
import re
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback
import urllib.request
import webbrowser
from pathlib import Path

import tkinter as tk
from tkinter import ttk, messagebox, filedialog, simpledialog

# ----------------------------------------------------------------------------
# 全局常量 / 配置
# ----------------------------------------------------------------------------
APP_NAME = "MCTier 局域网群组"
APP_VERSION = "1.1.0"        # 兜底版本号；实际以程序目录 update.wenyi 的第一行为准
DESKTOP_UPDATE_PAGE = "https://mctier.pmhs.top/#preview"   # 更新页（可在 config.json 修改）

# 版本清单文件：第一行版本号，其余各行是「更新内容」。本地一份，GitHub 上也放一份。
UPDATE_MANIFEST_NAME = "update.wenyi"

# GitHub 仓库（版本发布与克隆更新都用它）
GITHUB_OWNER = "ZZZ-need-sleep"
GITHUB_REPO = "Local_Network_Communication-created-by-DeepDeek-"
GITHUB_BRANCH = "main"          # 默认分支；若你的仓库默认分支是 master，请改这里
# 更新清单地址：直接读取 GitHub 上的 update.wenyi 作为校验文件
DEFAULT_UPDATE_URL = (f"https://raw.githubusercontent.com/{GITHUB_OWNER}/{GITHUB_REPO}"
                      f"/{GITHUB_BRANCH}/{UPDATE_MANIFEST_NAME}")
# 克隆更新用的仓库地址
DEFAULT_UPDATE_REPO = f"https://github.com/{GITHUB_OWNER}/{GITHUB_REPO}.git"
# 克隆更新时保存「用户所有会话内容」的文件夹名，以及克隆到的新目录名
CUSTOMER_DATA_DIR = "customer_god"
UPDATE_CLONE_DIR = "update_clone"

OS_IS_WINDOWS = (os.name == "nt")

# 数据目录（默认放在程序同目录 data/；测试可用环境变量 MCTIER_DATA_DIR 覆盖）
def get_data_dir() -> Path:
    env = os.environ.get("MCTIER_DATA_DIR")
    base = Path(env) if env else Path(__file__).resolve().parent / "data"
    base.mkdir(parents=True, exist_ok=True)
    return base

DATA_DIR = get_data_dir()


def update_root() -> Path:
    """克隆更新的工作目录（默认程序目录；测试可用 MCTIER_UPDATE_ROOT 覆盖）。"""
    env = os.environ.get("MCTIER_UPDATE_ROOT")
    return Path(env) if env else Path(__file__).resolve().parent


# 端口（config.json 可改；测试用环境变量覆盖）
DEFAULT_UDP_PORT = int(os.environ.get("MCTIER_UDP_PORT", "45455"))
DEFAULT_TCP_PORT = int(os.environ.get("MCTIER_TCP_PORT", "45431"))

# 心跳与在线判定（测试可用环境变量缩短）
PRESENCE_INTERVAL = float(os.environ.get("MCTIER_PRES_INTERVAL", "8"))     # 秒
PRESENCE_TTL      = float(os.environ.get("MCTIER_PRES_TTL", "30"))         # 秒，超过视为离线
OFFER_WAIT        = float(os.environ.get("MCTIER_OFFER_WAIT", "8"))        # 公共询问等待应答秒数
MAX_CHAT_LEN      = 4000
FILE_CHUNK        = 128 * 1024
LOG_MAX_LINES     = 800
SEEN_TTL          = 120   # UDP 去重窗口（秒）
FALLBACK_RETRY    = 2     # 直连失败后回退公共询问的次数上限

# 改名投票：第一次改名自由；之后需要「超过」该数目的用户投票同意（测试可用环境变量调整）
RENAME_VOTE_NEED  = int(os.environ.get("MCTIER_VOTE_NEED", "5"))      # 默认超过 5 票同意
RENAME_VOTE_WAIT  = float(os.environ.get("MCTIER_VOTE_WAIT", "120"))  # 投票等待秒数，超时未过则失败

SECRET_RELAY_RETRY = 3    # 转存送达失败（校验不通过等）的重试上限


# ----------------------------------------------------------------------------
# 小工具函数
# ----------------------------------------------------------------------------
def now_ts() -> float:
    return time.time()


def fmt_time(ts: float) -> str:
    try:
        return time.strftime("%m-%d %H:%M:%S", time.localtime(ts))
    except Exception:
        return ""


def human_size(n: float) -> str:
    n = float(n)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024
    return f"{n:.1f} TB"


def sanitize_name(s: str, maxlen: int = 120) -> str:
    s = re.sub(r'[\\/:*?"<>|\r\n\t]', "_", str(s)).strip()
    if not s:
        s = "未命名"
    return s[:maxlen]


def parse_update_manifest(data: str) -> dict:
    """解析更新清单，返回 {"version": str, "notes": str}。

    支持两种格式（开发者可任选）：
      1) JSON：{"version": "1.0.1", "notes": "..."}（notes 可为字符串或字符串数组）
      2) 纯文本：第一行为版本号，其余各行作为「更新内容」
    """
    data = (data or "").strip()
    if not data:
        return {"version": "", "notes": ""}
    # 先尝试 JSON 清单
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
    # 纯文本：首行版本号，其余为更新内容
    lines = data.splitlines()
    ver = lines[0].strip()
    notes = "\n".join(lines[1:]).strip()
    return {"version": ver, "notes": notes}


def local_manifest_path() -> Path:
    """本地版本清单 update.wenyi 的路径（程序目录下）。"""
    return Path(__file__).resolve().parent / UPDATE_MANIFEST_NAME


def load_local_manifest() -> dict:
    """读取本地 update.wenyi（失败则返回空清单，程序用兜底版本号）。"""
    try:
        return parse_update_manifest(local_manifest_path().read_text(encoding="utf-8"))
    except Exception:
        return {"version": "", "notes": ""}


def apply_local_version():
    """版本号以本地 update.wenyi 的第一行为准（发布新版本时只改这一个文件）。"""
    global APP_VERSION
    m = load_local_manifest()
    if m.get("version"):
        APP_VERSION = m["version"]


apply_local_version()

LOCAL_MANIFEST = load_local_manifest()


def get_local_ips() -> list:
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except Exception:
        pass
    ips.discard("127.0.0.1")
    return sorted(ips) or ["127.0.0.1"]


def compute_sha256(path: Path, progress=None) -> str:
    h = hashlib.sha256()
    total = path.stat().st_size if path.exists() else 0
    done = 0
    with open(path, "rb") as f:
        while True:
            blk = f.read(FILE_CHUNK)
            if not blk:
                break
            h.update(blk)
            done += len(blk)
            if progress and total > 0:
                progress(done / total)
    return h.hexdigest()


# ---------------------------------------------------------------- 保密转存加解密 --
# 纯标准库轻量加密：SHA-512 计数器模式 XOR（无第三方库，用于离线私聊转存，
# 让转存方 C 的磁盘上只有密文，避免其用户随手查看）；HMAC 用于完整性校验。
def secret_encrypt(text: str, key: bytes) -> str:
    data = text.encode("utf-8")
    out = bytearray()
    counter = 0
    i = 0
    while i < len(data):
        block = hashlib.sha512(key + counter.to_bytes(8, "big")).digest()
        for b in block:
            if i >= len(data):
                break
            out.append(data[i] ^ b)
            i += 1
        counter += 1
    return out.hex()


def secret_decrypt(hex_blob: str, key: bytes) -> str:
    data = bytes.fromhex(hex_blob)
    out = bytearray()
    counter = 0
    i = 0
    while i < len(data):
        block = hashlib.sha512(key + counter.to_bytes(8, "big")).digest()
        for b in block:
            if i >= len(data):
                break
            out.append(data[i] ^ b)
            i += 1
        counter += 1
    try:
        return out.decode("utf-8")
    except UnicodeDecodeError:
        raise ValueError("解密失败：密钥不正确或数据损坏")


def secret_sign(key: bytes, *parts) -> str:
    """对整包关键字段做 HMAC，用于接收方校验「发送的包是否完整/未被篡改」。"""
    h = hmac.new(key, b"", hashlib.sha256)
    for p in parts:
        h.update(str(p).encode("utf-8"))
        h.update(b"\x00")
    return h.hexdigest()


def open_with_system_app(path: Path):
    """用系统默认应用程序打开文件（跨平台）。"""
    try:
        if OS_IS_WINDOWS:
            os.startfile(str(path))                       # type: ignore[attr-defined]
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(path)])
        else:
            subprocess.Popen(["xdg-open", str(path)])
        return True
    except Exception:
        traceback.print_exc()
        return False


# ----------------------------------------------------------------------------
# 系统通知（Windows 用系统 API Shell_NotifyIcon 气泡；其他平台用 Tk 弹窗兜底）
# ----------------------------------------------------------------------------
class SystemNotifier:
    """调用系统 API 弹出右下角通知气泡。"""

    def __init__(self, root_widget=None):
        self._root = root_widget
        self._win_nid = None
        self._win_icon = None
        self._last_win_title = None

    # ---- Windows Shell_NotifyIcon 实现 -------------------------------------
    def _win_init(self):
        import ctypes.wintypes as wt

        class GUID(ctypes.Structure):
            _fields_ = [("Data1", wt.DWORD), ("Data2", wt.WORD),
                        ("Data3", wt.WORD), ("Data4", ctypes.c_ubyte * 8)]

        class NOTIFYICONDATAW(ctypes.Structure):
            _fields_ = [
                ("cbSize", wt.DWORD), ("hWnd", wt.HWND), ("uID", wt.UINT),
                ("uFlags", wt.UINT), ("uCallbackMessage", wt.UINT),
                ("hIcon", wt.HICON), ("szTip", wt.WCHAR * 128),
                ("dwState", wt.DWORD), ("dwStateMask", wt.DWORD),
                ("szInfo", wt.WCHAR * 256),
                ("uTimeoutOrVersion", wt.UINT),       # 联合：uTimeout / uVersion
                ("szInfoTitle", wt.WCHAR * 64),
                ("dwInfoFlags", wt.DWORD), ("guidItem", GUID),
                ("hBalloonIcon", wt.HICON),
            ]

        self._GUID = GUID
        self._NID = NOTIFYICONDATAW
        self._shell32 = ctypes.windll.shell32
        self._user32 = ctypes.windll.user32

    def _win_notify(self, title: str, msg: str) -> bool:
        try:
            if self._win_nid is None:
                self._win_init()
                hwnd = self._root.winfo_id() if (self._root is not None) else 0
                nid = self._NID()
                nid.cbSize = ctypes.sizeof(self._NID)
                nid.hWnd = hwnd
                nid.uID = 777
                nid.uFlags = 0x00000001 | 0x00000002 | 0x00000004   # MESSAGE|ICON|TIP
                nid.uCallbackMessage = 0x0400 + 50                   # WM_APP+50
                nid.hIcon = self._user32.LoadIconW(0, 32512)         # IDI_APPLICATION
                nid.szTip = APP_NAME[:127]
                ok = self._shell32.Shell_NotifyIconW(0x00000000, ctypes.byref(nid))  # NIM_ADD
                if not ok:
                    return False
                self._win_nid = nid
                self._win_icon = nid.hIcon
            nid = self._win_nid
            nid.uFlags = 0x00000004 | 0x00000010                     # TIP | INFO
            nid.szInfoTitle = str(title)[:63]
            nid.szInfo = str(msg)[:254]
            nid.dwInfoFlags = 0x00000001                             # NIIF_INFO
            return bool(self._shell32.Shell_NotifyIconW(0x00000001, ctypes.byref(nid)))  # NIM_MODIFY
        except Exception:
            traceback.print_exc()
            return False

    def _tk_popup(self, title: str, msg: str):
        try:
            if not self._root:
                return
            top = tk.Toplevel(self._root)
            top.title(title)
            top.attributes("-topmost", True)
            top.geometry("+%d+%d" % (self._root.winfo_rootx() + 60,
                                     self._root.winfo_rooty() + 60))
            tk.Label(top, text=msg, wraplength=340, justify="left",
                     padx=14, pady=12).pack()
            top.after(6000, top.destroy)
        except Exception:
            pass

    def notify(self, title: str, msg: str):
        """发送系统通知；失败时退回 Tk 弹窗。"""
        if OS_IS_WINDOWS:
            if self._win_notify(title, msg):
                return
        self._tk_popup(title, msg)


# ----------------------------------------------------------------------------
# 协议 / 传输层
# ----------------------------------------------------------------------------
def encode_frame(payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + payload


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        blk = sock.recv(n - len(buf))
        if not blk:
            raise ConnectionError("连接被对端关闭")
        buf += blk
    return buf


def send_json(sock: socket.socket, obj: dict):
    sock.sendall(encode_frame(json.dumps(obj, ensure_ascii=False).encode("utf-8")))


def recv_json(sock: socket.socket) -> dict:
    hdr = recv_exact(sock, 4)
    n = struct.unpack(">I", hdr)[0]
    if n > 64 * 1024 * 1024:
        raise ValueError("异常的数据帧长度")
    return json.loads(recv_exact(sock, n).decode("utf-8"))


# ----------------------------------------------------------------------------
# 配置读写
# ----------------------------------------------------------------------------
def load_config() -> dict:
    cfg_path = DATA_DIR / "config.json"
    cfg = {
        "name": os.environ.get("MCTIER_NAME") or socket.gethostname(),
        "uid": "PENDING",
        "tcp_port": DEFAULT_TCP_PORT,
        "udp_port": DEFAULT_UDP_PORT,
        "groups": [],            # [{"gid":..., "gname":...}]
        "update_url": DEFAULT_UPDATE_URL,
        "update_page": DESKTOP_UPDATE_PAGE,
        "update_repo": DEFAULT_UPDATE_REPO,     # 克隆更新的仓库地址（GitHub）
        "saved_files": [],       # 记忆群文件（仅展示用）
        "rename_count": 0,       # 已成功改名次数（第一次改名自由，之后需投票）
        "rename_vote_need": RENAME_VOTE_NEED,   # 之后改名需「超过」的同意票数
    }
    if cfg_path.exists():
        try:
            data = json.loads(cfg_path.read_text(encoding="utf-8"))
            for k, v in data.items():
                cfg[k] = v
        except Exception:
            pass
    # 生成或读取持久唯一 ID（优先物理地址 MAC，其次随机）
    if not cfg.get("uid") or cfg["uid"] == "PENDING":
        cfg["uid"] = make_uid()
    cfg.setdefault("name", socket.gethostname())
    cfg.setdefault("tcp_port", DEFAULT_TCP_PORT)
    cfg.setdefault("udp_port", DEFAULT_UDP_PORT)
    cfg.setdefault("groups", [])
    cfg.setdefault("update_url", DEFAULT_UPDATE_URL)
    cfg.setdefault("update_page", DESKTOP_UPDATE_PAGE)
    cfg.setdefault("update_repo", DEFAULT_UPDATE_REPO)
    cfg.setdefault("rename_count", 0)
    cfg.setdefault("rename_vote_need", RENAME_VOTE_NEED)
    save_config(cfg)
    return cfg


def make_uid() -> str:
    """唯一 ID：优先使用物理网卡 MAC 地址（12 位十六进制）。"""
    try:
        mac = uuid_getnode_mac()
        if mac:
            return mac
    except Exception:
        pass
    return "RND" + secrets.token_hex(6).upper()


def uuid_getnode_mac() -> str:
    mac = uuid_getnode()
    # uuid_getnode() 在多播位为 1 时表示随机，此时不用
    if (mac >> 40) & 0x1:
        return ""
    return f"{mac:012X}"


def uuid_getnode() -> int:
    import uuid as _uuid
    return _uuid.getnode()


def save_config(cfg: dict):
    try:
        (DATA_DIR / "config.json").write_text(
            json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
    except Exception:
        pass


# ----------------------------------------------------------------------------
# 群组核心：PeerCore
#   - UDP 广播：presence / chat / file_pub / file_req / file_have / log / sync
#   - TCP 服务：按文件 ID 提供下载（点对点）
#   - 下载调度：直接下载 -> 失败回退公共询问 -> 第一个应答者的 IP
#   - 事件通过 queue 交给 GUI，核心不依赖 GUI
# ----------------------------------------------------------------------------
class PeerCore:
    def __init__(self, cfg: dict, events: queue.Queue, name_getter=None, data_root: Path = None):
        self.cfg = cfg
        self.uid = cfg["uid"]
        self.events = events
        self._name_getter = name_getter or (lambda: cfg["name"])
        self.data_root = Path(data_root) if data_root else DATA_DIR

        self.groups = {g["gid"]: {"gid": g["gid"], "gname": g["gname"]}
                       for g in cfg.get("groups", [])}

        self.peers = {}          # uid -> {"name","ip","tcp_port","groups","ver","ts"}
        self.discovered = {}     # gid -> {"gname","host_uid","host_name","ts"}
        self._seen = {}          # msg_id -> ts（UDP 去重）
        self._msg_counter = 0

        self.index = {}          # file_id -> {"name","size","ts","src_uid","src_name","groups":[],"has_local"}
        self._load_index()

        # 保密转存（替别人暂存的离线私聊 / 我已收到的转存编号）
        self._secrets = {}        # store_id -> 转存记录（加密密文 + 密钥 + 签名）
        self._recv_secrets = set()  # 我已收到的 store_id（去重，避免重复显示）
        self._delivering = set()    # 正在送达的目标 uid（防止并发重复送达）
        self._load_secrets()
        self._load_recv_secrets()

        self._offers = {}        # req_id -> [(ip, port, uid)]
        self._downloading = set()
        self._download_queue = queue.Queue()
        self._active_downloads = {}   # file_id -> task dict（供 GUI 查询进度）

        self._running = True
        self._lock = threading.RLock()
        self._udp_sock = None
        self._tcp_srv = None
        self._threads = []

        self.file_offers = []    # 我下载过的文件记录（用于回退公共询问应答）

        # 改名投票状态
        self._rename_votes = {}      # pid -> {"old","new","approvers":set(),"rejectors":set(),"done","deadline"}
        self._rename_requests = set()  # 已向本机用户展示过的改名提案 pid（去重弹窗）
        self._my_votes = set()         # 本机用户已投过票的提案 pid（同一提案只计一票）

    # ------------------------------------------------------------- 基本属性 --
    @property
    def my_name(self) -> str:
        return str(self._name_getter())

    def my_groups(self) -> list:
        return list(self.groups.values())

    # ------------------------------------------------------------- 启动/停止 --
    def start(self):
        self._udp_sock = self._make_udp()
        self._tcp_srv = self._make_tcp()
        workers = [
            ("udp", self._udp_loop),
            ("tcp", self._tcp_loop),
            ("presence", self._presence_loop),
            ("sweep", self._sweep_loop),
            ("download", self._download_worker),
        ]
        for _name, fn in workers:
            t = threading.Thread(target=self._safe(fn), name=f"core-{_name}", daemon=True)
            t.start()
            self._threads.append(t)
        self._emit("syslog", f"程序启动完成，局域网地址 {', '.join(get_local_ips())}")

    def stop(self):
        self._running = False
        try:
            if self._udp_sock:
                self._udp_sock.close()
        except Exception:
            pass
        for t in self._threads:
            t.join(timeout=1.5)

    def _safe(self, fn):
        def wrap(*a, **kw):
            try:
                return fn(*a, **kw)
            except Exception:
                if self._running:
                    traceback.print_exc()
                return None
        return wrap

    def _emit(self, ev: str, *payload):
        try:
            self.events.put((ev,) + payload)
        except Exception:
            pass

    # ------------------------------------------------------------- 网络建立 --
    def _make_udp(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            s.bind(("", int(self.cfg.get("udp_port", DEFAULT_UDP_PORT))))
        except OSError:
            s.bind(("", 0))
        s.settimeout(1.0)
        return s

    def _make_tcp(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        port = int(self.cfg.get("tcp_port", DEFAULT_TCP_PORT))
        for p in (port, ) + tuple(range(port + 1, port + 21)):
            try:
                s.bind(("0.0.0.0", p))
                s.listen(16)
                if p != port:
                    self.cfg["tcp_port"] = p
                    save_config(self.cfg)
                break
            except OSError:
                continue
        else:
            raise RuntimeError("无法绑定 TCP 端口")
        return s

    def my_tcp_port(self) -> int:
        return self._tcp_srv.getsockname()[1] if self._tcp_srv else int(self.cfg.get("tcp_port", DEFAULT_TCP_PORT))

    # ------------------------------------------------------------- 发送广播 --
    def _broadcast(self, msg: dict, addr=None):
        msg.setdefault("uid", self.uid)
        msg.setdefault("name", self.my_name)
        msg.setdefault("msg_id", self._new_msg_id())
        try:
            data = json.dumps(msg, ensure_ascii=False).encode("utf-8")
            if addr:
                self._udp_sock.sendto(data, addr)
                return
            targets = {"255.255.255.255:udp", }
            for ip in get_local_ips():
                if ip.count(".") == 3:
                    targets.add(ip.rsplit(".", 1)[0] + ".255")
                targets.add(ip + ":udp")
            for t in list(targets):
                host = t.replace(":udp", "")
                try:
                    self._udp_sock.sendto(data, (host, int(self.cfg.get("udp_port", DEFAULT_UDP_PORT))))
                except OSError:
                    pass
        except Exception:
            if self._running:
                traceback.print_exc()

    def _new_msg_id(self) -> str:
        self._msg_counter += 1
        return f"{self.uid}-{self._msg_counter}-{secrets.token_hex(3)}"

    # ------------------------------------------------------------- UDP 循环 --
    def _udp_loop(self):
        while self._running:
            try:
                data, addr = self._udp_sock.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                msg = json.loads(data.decode("utf-8"))
            except Exception:
                continue
            if not isinstance(msg, dict):
                continue
            if msg.get("uid") == self.uid:
                continue   # 自己的广播直接忽略
            self._dispatch_udp(msg, addr)

    def _dispatch_udp(self, msg: dict, addr):
        kind = msg.get("t")
        mid = msg.get("msg_id")
        if mid:
            if mid in self._seen:
                return
            self._seen[mid] = now_ts()
            if len(self._seen) > 4000:
                self._prune_seen()
        if kind == "presence":
            self._on_presence(msg, addr)
        elif kind == "chat":
            self._on_chat(msg)
        elif kind == "file_pub":
            self._on_file_pub(msg)
        elif kind == "file_req":
            self._on_file_req(msg, addr)
        elif kind == "file_have":
            self._on_file_have(msg)
        elif kind == "file_sync_req":
            self._on_sync_req(msg)
        elif kind == "log":
            self._on_log(msg)
        elif kind == "group_announce":
            self._on_group_announce(msg)
        elif kind == "rename_req":
            self._on_rename_req(msg)
        elif kind == "rename_vote":
            self._on_rename_vote(msg)
        elif kind == "dm":
            self._on_dm(msg)

    def _prune_seen(self):
        ts = now_ts()
        for k in [k for k, v in self._seen.items() if ts - v > SEEN_TTL]:
            self._seen.pop(k, None)

    # ------------------------------------------------------------- 在线表 --
    def _on_presence(self, msg: dict, addr):
        uid = msg.get("uid")
        if not uid:
            return
        self.peers[uid] = {
            "name": str(msg.get("name", "")),
            "ip": msg.get("ip") or addr[0],
            "tcp_port": int(msg.get("port") or 0),
            "groups": msg.get("groups", []) or [],
            "ver": msg.get("ver", ""),
            "ts": now_ts(),
        }
        for g in self.peers[uid]["groups"]:
            gid = g.get("gid")
            if gid:
                self.discovered.setdefault(gid, {
                    "gname": g.get("gname", f"群{gid[:6]}"),
                    "host_uid": uid,
                    "host_name": self.peers[uid]["name"],
                    "ts": now_ts(),
                })
        self._on_presence_hook_deliver(uid)
        self._emit("peers_changed")

    def _on_group_announce(self, msg: dict):
        gid = msg.get("gid")
        if not gid:
            return
        self.discovered[gid] = {
            "gname": msg.get("gname", f"群{gid[:6]}"),
            "host_uid": msg.get("uid"),
            "host_name": msg.get("name", ""),
            "ts": now_ts(),
        }
        self._emit("peers_changed")

    def is_online(self, uid: str) -> bool:
        p = self.peers.get(uid)
        return bool(p) and (now_ts() - p["ts"] <= PRESENCE_TTL) and p.get("tcp_port")

    def group_members(self, gid: str) -> list:
        """查询某群当前在线成员列表（含自己）。"""
        out = [{"name": self.my_name, "uid": self.uid, "ip": "本机", "self": True}]
        ts = now_ts()
        for uid, p in self.peers.items():
            if now_ts() - (p.get("ts") or 0) > PRESENCE_TTL:
                continue
            if any(g.get("gid") == gid for g in p.get("groups", [])):
                out.append({"name": p["name"], "uid": uid, "ip": p["ip"], "self": False})
        return out

    # ------------------------------------------------------------- 心跳循环 --
    def _presence_loop(self):
        while self._running:
            try:
                self._broadcast({
                    "t": "presence",
                    "ip": get_local_ips()[0],
                    "port": self.my_tcp_port(),
                    "groups": [{"gid": g["gid"], "gname": g["gname"]} for g in self.my_groups()],
                    "ver": APP_VERSION,
                })
            except Exception:
                pass
            time.sleep(PRESENCE_INTERVAL)

    def _sweep_loop(self):
        while self._running:
            time.sleep(3)
            ts = now_ts()
            changed = False
            for uid in list(self.peers):
                if ts - self.peers[uid]["ts"] > PRESENCE_TTL * 2:
                    del self.peers[uid]
                    changed = True
            for gid in list(self.discovered):
                if ts - self.discovered[gid]["ts"] > PRESENCE_TTL * 4:
                    del self.discovered[gid]
                    changed = True
            if changed:
                self._emit("peers_changed")

    # ------------------------------------------------------------- 聊天 --
    def send_chat(self, gid: str, text: str):
        text = str(text).strip()[:MAX_CHAT_LEN]
        if not text:
            return
        g = self.groups.get(gid)
        if not g:
            return
        self._broadcast({
            "t": "chat", "gid": gid, "gname": g["gname"],
            "text": text, "ts": now_ts(),
        })
        self._emit("chat", {"gid": gid, "gname": g["gname"], "from_name": self.my_name,
                            "from_uid": self.uid, "text": text, "ts": now_ts(), "self": True})

    def _on_chat(self, msg: dict):
        gid = msg.get("gid")
        if gid not in self.groups:
            return
        self._emit("chat", {"gid": gid, "gname": msg.get("gname", ""),
                            "from_name": msg.get("name", "?"), "from_uid": msg.get("uid"),
                            "text": str(msg.get("text", ""))[:MAX_CHAT_LEN],
                            "ts": msg.get("ts", now_ts()), "self": False})

    # ------------------------------------------------------------- 双人私聊 --
    def send_dm(self, to_uid: str, text: str) -> bool:
        """给指定用户发私聊（广播携带 to_uid，只有收件人处理）。"""
        text = str(text).strip()[:MAX_CHAT_LEN]
        to_uid = str(to_uid or "")
        if not text or not to_uid or to_uid == self.uid:
            return False
        self._broadcast({"t": "dm", "to_uid": to_uid, "text": text, "ts": now_ts()})
        self._emit("dm", {"to_uid": to_uid, "from_uid": self.uid, "from_name": self.my_name,
                          "text": text, "ts": now_ts(), "self": True})
        return True

    def _on_dm(self, msg: dict):
        """收到私聊：不是发给自己的静默忽略（不记日志、不打扰）。"""
        if msg.get("to_uid") != self.uid:
            return
        self._emit("dm", {"to_uid": self.uid, "from_uid": str(msg.get("uid") or ""),
                          "from_name": str(msg.get("name", "?")),
                          "text": str(msg.get("text", ""))[:MAX_CHAT_LEN],
                          "ts": msg.get("ts", now_ts()), "self": False})

    # ------------------------------------------------------------- 保密转存（离线私聊接力）--
    def send_dm_auto(self, to_uid: str, text: str) -> dict:
        """自动私聊：对方在线 -> 直接发送；离线 -> 加密转存给某位在线同学，等对方上线自动送达。
        离线/失败的结果通过 'dm_sent' 事件回报 GUI（直接发送成功沿用 dm 回显，不再额外回报）。"""
        text = str(text).strip()[:MAX_CHAT_LEN]
        to_uid = str(to_uid or "")
        if not text or not to_uid or to_uid == self.uid:
            return {"ok": False, "reason": "参数无效"}
        if self.is_online(to_uid):
            self.send_dm(to_uid, text)
            return {"ok": True, "direct": True}
        t = threading.Thread(target=self._safe(self._relay_dm_worker), args=(to_uid, text),
                             name="dm-relay", daemon=True)
        t.start()
        self._threads.append(t)
        return {"ok": True, "pending": True}

    def _relay_dm_worker(self, to_uid: str, text: str):
        cands = [p for uid, p in self.peers.items()
                 if uid not in (to_uid, self.uid) and self.is_online(uid)]
        random.shuffle(cands)
        for p in cands:
            store_id = self._new_msg_id()
            key = secrets.token_bytes(32)
            rec = {"to_uid": to_uid, "from_uid": self.uid, "from_name": self.my_name,
                   "blob": secret_encrypt(text, key), "key": key.hex(),
                   "sig": secret_sign(key, to_uid, self.uid, store_id, text),
                   "ts": now_ts(), "retries": 0}
            if self._store_secret_on(p, store_id, rec):
                self._emit("dm_sent", {"to_uid": to_uid, "ok": True, "direct": False,
                                       "relay_name": str(p.get("name", "?")),
                                       "text": text, "ts": rec["ts"]})
                return
        self._emit("dm_sent", {"to_uid": to_uid, "ok": False,
                               "reason": "对方不在线，且没有其他在线同学可代为转存"})

    def _store_secret_on(self, peer: dict, store_id: str, rec: dict) -> bool:
        ip, port = peer.get("ip"), peer.get("tcp_port")
        if not ip or not port:
            return False
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(6)
        try:
            sock.connect((ip, int(port)))
            send_json(sock, {"cmd": "secret_store", "store_id": store_id, **rec})
            resp = recv_json(sock)
            return bool(resp.get("cmd") == "secret_stored" and resp.get("store_id") == store_id)
        except Exception:
            return False
        finally:
            try:
                sock.close()
            except Exception:
                pass

    def _on_presence_hook_deliver(self, uid: str):
        """某用户上线（收到 presence）时，把替他保管的转存私聊送达。"""
        if uid in self._delivering:
            return
        self._delivering.add(uid)
        t = threading.Thread(target=self._safe(self._deliver_secrets_to), args=(uid,),
                             name="secret-deliver", daemon=True)
        t.start()
        self._threads.append(t)

    def _deliver_secrets_to(self, uid: str):
        try:
            entries = [(sid, rec) for sid, rec in list(self._secrets.items())
                       if rec.get("to_uid") == uid]
            if not entries:
                return
            p = self.peers.get(uid, {})
            for sid, rec in entries:
                ok = self._deliver_one(p, sid, rec)
                with self._lock:
                    if ok:
                        self._secrets.pop(sid, None)
                    else:
                        rec["retries"] = int(rec.get("retries", 0)) + 1
                        if rec["retries"] > SECRET_RELAY_RETRY:
                            self._secrets.pop(sid, None)
            self._save_secrets()
        finally:
            self._delivering.discard(uid)

    def _deliver_one(self, peer: dict, store_id: str, rec: dict) -> bool:
        ip, port = peer.get("ip"), peer.get("tcp_port")
        if not ip or not port:
            return False
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(8)
        try:
            sock.connect((ip, int(port)))
            send_json(sock, {"cmd": "secret", "store_id": store_id,
                             "to_uid": rec.get("to_uid"), "from_uid": rec.get("from_uid"),
                             "from_name": rec.get("from_name"), "blob": rec.get("blob"),
                             "key": rec.get("key"), "sig": rec.get("sig"), "ts": rec.get("ts")})
            resp = recv_json(sock)
            return bool(resp.get("cmd") == "secret_ack" and resp.get("ok"))
        except Exception:
            return False
        finally:
            try:
                sock.close()
            except Exception:
                pass

    def _on_secret_store(self, req: dict):
        """替别人暂存一条转存私聊（纯机器行为：不进 UI、不进日志、不产生事件）。"""
        sid = str(req.get("store_id") or "")
        if not sid or not req.get("to_uid") or not req.get("blob") or not req.get("key"):
            return
        if sid in self._secrets:
            return
        self._secrets[sid] = {
            "to_uid": str(req["to_uid"]), "from_uid": str(req.get("from_uid") or ""),
            "from_name": str(req.get("from_name") or ""), "blob": str(req["blob"]),
            "key": str(req["key"]), "sig": str(req.get("sig") or ""),
            "ts": req.get("ts", now_ts()), "retries": 0,
        }
        self._save_secrets()

    def _on_secret_deliver(self, req: dict) -> bool:
        """收到转存送达：解密并校验完整性，通过后显示并回执（让转存方删除）。"""
        sid = str(req.get("store_id") or "")
        if req.get("to_uid") != self.uid:
            return False
        if sid and sid in self._recv_secrets:
            return True   # 已收过：幂等回执，避免重复显示
        try:
            key = bytes.fromhex(str(req.get("key") or ""))
            text = secret_decrypt(str(req.get("blob") or ""), key)
            sig_ok = hmac.compare_digest(
                str(req.get("sig") or ""),
                secret_sign(key, req.get("to_uid"), req.get("from_uid"), sid, text))
        except Exception:
            return False
        if not sig_ok:
            return False
        if sid:
            self._recv_secrets.add(sid)
            if len(self._recv_secrets) > 2000:
                self._recv_secrets = set(sorted(self._recv_secrets)[-2000:])
            self._save_recv_secrets()
        self._emit("dm", {"to_uid": self.uid, "from_uid": str(req.get("from_uid") or ""),
                          "from_name": str(req.get("from_name") or "?"),
                          "text": text, "ts": req.get("ts", now_ts()),
                          "self": False, "relayed": True})
        return True

    # ------------------------------------------------------------- 群组管理 --
    def create_group(self, gname: str) -> dict:
        gname = sanitize_name(gname, 40)
        gid = secrets.token_hex(3).upper()
        g = {"gid": gid, "gname": gname}
        self.groups[gid] = g
        self._persist_groups()
        self._broadcast({"t": "group_announce", "gid": gid, "gname": gname})
        self.broadcast_log(f"「{self.my_name}」创建了群「{gname}」")
        self._sync_req(gid)     # 创建后请求群文件列表（有朋友发过文件时可补收）
        self._emit("groups_changed")
        return g

    def join_group(self, gid: str, gname: str = "") -> dict:
        gid = str(gid).strip().upper()
        if not gid:
            raise ValueError("群号不能为空")
        if gid in self.groups:
            return self.groups[gid]
        if not gname:
            gname = self.discovered.get(gid, {}).get("gname", f"群{gid[:6]}")
        g = {"gid": gid, "gname": sanitize_name(gname, 40)}
        self.groups[gid] = g
        self._persist_groups()
        self.broadcast_log(f"「{self.my_name}」加入了群「{g['gname']}」")
        self._sync_req(gid)
        self._emit("groups_changed")
        return g

    def leave_group(self, gid: str):
        g = self.groups.pop(gid, None)
        if g:
            self._persist_groups()
            self.broadcast_log(f"「{self.my_name}」退出了群「{g['gname']}」")
            self._emit("groups_changed")

    def _persist_groups(self):
        self.cfg["groups"] = [{"gid": g["gid"], "gname": g["gname"]} for g in self.my_groups()]
        save_config(self.cfg)

    # ------------------------------------------------------------- 改名（首次自由，之后需投票）--
    @property
    def vote_need(self) -> int:
        """之后改名需要「超过」的同意票数（默认超过 5 票，可在 config.json 调整）。"""
        try:
            return int(self.cfg.get("rename_vote_need", RENAME_VOTE_NEED))
        except Exception:
            return RENAME_VOTE_NEED

    @property
    def rename_count(self) -> int:
        """本机已经成功改名的次数（第一次改名自由）。"""
        try:
            return int(self.cfg.get("rename_count", 0) or 0)
        except Exception:
            return 0

    def rename_needs_vote(self) -> bool:
        """是否已改过一次名字（之后修改需投票通过）。"""
        return self.rename_count > 0

    def rename(self, new_name: str):
        """第一次改名直接生效；之后再改需先发起投票（见 start_rename_vote）。"""
        new_name = sanitize_name(new_name, 40)
        old = self.my_name
        if new_name == old:
            return
        if self.rename_needs_vote():
            raise PermissionError(
                f"已改过一次名字，之后的修改需要超过 {self.vote_need} 名用户投票同意")
        self._apply_rename(new_name, old)

    def _apply_rename(self, new_name: str, old: str, note: str = ""):
        self.cfg["name"] = new_name
        self.cfg["rename_count"] = self.rename_count + 1
        save_config(self.cfg)
        self.broadcast_log(f"「{old}」更名为「{new_name}」{note}".rstrip())
        self._emit("name_changed", new_name)

    def start_rename_vote(self, new_name: str) -> str:
        """发起改名投票：广播提案，超过 vote_need 名用户同意后自动生效。返回提案 pid。"""
        new_name = sanitize_name(new_name, 40)
        old = self.my_name
        if new_name == old:
            raise ValueError("新名字与当前名字相同")
        if not self.rename_needs_vote():
            raise PermissionError("第一次改名无需投票，请直接保存")
        with self._lock:
            if any(not v["done"] for v in self._rename_votes.values()):
                raise RuntimeError("已有一个改名投票正在进行，请等待结果")
        pid = self._new_msg_id()
        with self._lock:
            self._rename_votes[pid] = {
                "old": old, "new": new_name,
                "approvers": set(), "rejectors": set(),
                "done": False, "deadline": now_ts() + RENAME_VOTE_WAIT,
            }
        self._broadcast({"t": "rename_req", "pid": pid, "old": old, "new": new_name,
                         "ts": now_ts()})
        self.broadcast_log(f"「{old}」申请更名为「{new_name}」："
                           f"需要超过 {self.vote_need} 名用户投票同意后生效")
        self._emit("rename_vote_progress", pid, 0, 0)
        t = threading.Thread(target=self._safe(self._rename_vote_waiter), args=(pid,),
                             name="rename-vote", daemon=True)
        t.start()
        self._threads.append(t)
        return pid

    def _rename_vote_waiter(self, pid: str):
        v = self._rename_votes.get(pid)
        if not v:
            return
        while self._running and not v["done"] and now_ts() < v["deadline"]:
            time.sleep(0.3)
        self._finish_rename_vote(pid)

    def _finish_rename_vote(self, pid: str):
        with self._lock:
            v = self._rename_votes.get(pid)
            if not v or v["done"]:
                return
            v["done"] = True
        approves = len(v["approvers"])
        if approves > self.vote_need:
            self._apply_rename(v["new"], v["old"], note=f"（{approves} 名用户投票通过）")
            self._emit("rename_vote_done", pid, True,
                       f"投票通过：{approves} 名用户同意（需超过 {self.vote_need} 名）", v["new"])
        else:
            self._emit("rename_vote_done", pid, False,
                       f"同意票不足：收到 {approves} 票，需超过 {self.vote_need} 票", v["new"])
        with self._lock:
            self._rename_votes.pop(pid, None)

    def cast_rename_vote(self, pid: str, approve: bool):
        """作为投票人对某个改名提案投票（同一台电脑对同一提案只计一票）。"""
        pid = str(pid)
        if not pid or pid in self._my_votes:
            return
        if len(self._my_votes) > 1000:
            self._my_votes.clear()
        self._my_votes.add(pid)
        self._broadcast({"t": "rename_vote", "pid": pid,
                         "approve": 1 if approve else 0, "ts": now_ts()})

    def _on_rename_req(self, msg: dict):
        """收到改名提案：交给本机用户投票（GUI 弹窗）。"""
        pid = msg.get("pid")
        if not pid or pid in self._rename_requests:
            return
        if len(self._rename_requests) > 1000:
            self._rename_requests.clear()
        self._rename_requests.add(pid)
        try:
            if now_ts() - float(msg.get("ts") or 0) > RENAME_VOTE_WAIT + 10:
                return   # 过期提案不再打扰用户
        except Exception:
            pass
        self._emit("rename_request", {
            "pid": pid, "from_uid": str(msg.get("uid") or ""),
            "from_name": str(msg.get("name", "?")),
            "old": str(msg.get("old", "")), "new": str(msg.get("new", "")),
        })

    def _on_rename_vote(self, msg: dict):
        """提案人收到投票：按投票人 uid 去重，同意票超过阈值立即通过。"""
        pid = msg.get("pid")
        with self._lock:
            v = self._rename_votes.get(pid)
            if not v or v["done"]:
                return
            uid = str(msg.get("uid") or "")
            if not uid or uid in v["approvers"] or uid in v["rejectors"]:
                return
            if msg.get("approve"):
                v["approvers"].add(uid)
            else:
                v["rejectors"].add(uid)
            approves = len(v["approvers"])
            rejects = len(v["rejectors"])
        self._emit("rename_vote_progress", pid, approves, rejects)
        if approves > self.vote_need:
            self._finish_rename_vote(pid)

    # ------------------------------------------------------------- 公共日志 --
    def broadcast_log(self, text: str):
        """向全网广播一条日志（所有人可见、只读）。"""
        self._broadcast({"t": "log", "text": str(text)[:300], "ts": now_ts()})
        self._emit("log", f"[{fmt_time(now_ts())}] {self.my_name}: {text}")

    def _on_log(self, msg: dict):
        self._emit("log", f"[{fmt_time(msg.get('ts', now_ts()))}] "
                          f"{msg.get('name', '?')}: {msg.get('text', '')}")

    # ------------------------------------------------------------- 文件系统 --
    def cache_dir(self) -> Path:
        d = self.data_root / "cache"
        d.mkdir(parents=True, exist_ok=True)
        return d

    def downloads_dir(self, gid: str, gname: str) -> Path:
        d = self.data_root / "downloads" / f"{sanitize_name(gname, 30)}_{gid[:6]}"
        d.mkdir(parents=True, exist_ok=True)
        return d

    def index_path(self) -> Path:
        return self.data_root / "index.json"

    def _load_index(self):
        try:
            if self.index_path().exists():
                self.index = json.loads(self.index_path().read_text(encoding="utf-8"))
        except Exception:
            self.index = {}

    def _save_index(self):
        try:
            self.index_path().write_text(
                json.dumps(self.index, ensure_ascii=False, indent=1), encoding="utf-8")
        except Exception:
            pass

    def _secrets_path(self) -> Path:
        return self.data_root / "secret_relay.json"

    def _recv_path(self) -> Path:
        return self.data_root / "secret_received.json"

    def _load_secrets(self):
        try:
            if self._secrets_path().exists():
                self._secrets = json.loads(self._secrets_path().read_text(encoding="utf-8"))
        except Exception:
            self._secrets = {}

    def _save_secrets(self):
        try:
            self._secrets_path().write_text(
                json.dumps(self._secrets, ensure_ascii=False), encoding="utf-8")
        except Exception:
            pass

    def _load_recv_secrets(self):
        try:
            if self._recv_path().exists():
                self._recv_secrets = set(json.loads(self._recv_path().read_text(encoding="utf-8")))
        except Exception:
            self._recv_secrets = set()

    def _save_recv_secrets(self):
        try:
            self._recv_path().write_text(
                json.dumps(sorted(self._recv_secrets), ensure_ascii=False), encoding="utf-8")
        except Exception:
            pass

    def has_file(self, file_id: str) -> bool:
        rec = self.index.get(file_id)
        if rec and rec.get("has_local"):
            p = self.cache_dir() / file_id
            if p.is_file():
                return True
        return False

    def index_record(self, file_id):
        return self.index.get(file_id)

    def send_file_to_group(self, path: Path, gid: str):
        """把本机文件发送到群：入库 + 广播 file_pub，组内成员自动下载。"""
        path = Path(path)
        if not path.is_file():
            raise ValueError("文件不存在")
        g = self.groups.get(gid)
        if not g:
            raise ValueError("群组不存在")
        fsize = path.stat().st_size
        self._emit("syschat", gid, f"正在计算文件指纹（{human_size(fsize)}）...")
        fid = compute_sha256(path)
        # 复制进缓存（作为发送者，提供服务）
        cache = self.cache_dir() / fid
        if not cache.exists():
            import shutil
            shutil.copy2(path, cache)
        self._register_file(fid, {
            "name": path.name, "size": fsize, "ts": now_ts(),
            "src_uid": self.uid, "src_name": self.my_name,
            "groups": list({gid}), "has_local": True,
        })
        self._broadcast({
            "t": "file_pub", "gid": gid, "gname": g["gname"],
            "file": {"id": fid, "name": path.name, "size": fsize}, "ts": now_ts(),
            "sender_uid": self.uid, "sender_name": self.my_name,
        })
        self.broadcast_log(f"「{self.my_name}」在群「{g['gname']}」发送了文件「{path.name}」")
        self._emit("file_new", {"file": self._file_view(fid), "gid": gid, "from_self": True})

    def _register_file(self, fid: str, rec: dict):
        old = self.index.get(fid, {})
        rec.setdefault("groups", [])
        rec["groups"] = sorted(set(old.get("groups", [])) | set(rec.get("groups", [])))
        self.index[fid] = rec
        self._save_index()

    def _file_view(self, fid: str) -> dict:
        r = self.index.get(fid, {})
        return {
            "id": fid, "name": r.get("name", "?"), "size": r.get("size", 0),
            "src_uid": r.get("src_uid", ""), "src_name": r.get("src_name", ""),
            "ts": r.get("ts", 0), "groups": r.get("groups", []),
            "has_local": bool(r.get("has_local")) and (self.cache_dir() / fid).is_file(),
        }

    def files_of_group(self, gid: str) -> list:
        return [self._file_view(fid) for fid, r in self.index.items()
                if gid in r.get("groups", [])]

    # ------------------------------------------------------------- 收到文件公告 --
    def _on_file_pub(self, msg: dict):
        gid = msg.get("gid")
        if gid not in self.groups:
            return
        f = msg.get("file", {})
        fid = f.get("id")
        if not fid:
            return
        gname = msg.get("gname", self.groups[gid]["gname"])
        # 使用原始发送人信息（转发 / 重播时保持一致，确保通知显示「原作者」）
        sender = msg.get("sender_name") or msg.get("name", "?")
        sender_uid = msg.get("sender_uid") or msg.get("uid")
        # 公共日志：全部可见
        self._emit("log", f"[{fmt_time(msg.get('ts', now_ts()))}] {sender}: "
                          f"在群「{gname}」发送了文件「{f.get('name', '?')}」")
        self._emit("file_new", {
            "file": {"id": fid, "name": f.get("name", "?"), "size": f.get("size", 0),
                     "src_uid": sender_uid, "src_name": sender, "ts": msg.get("ts", now_ts()),
                     "groups": [gid], "has_local": self.has_file(fid)},
            "gid": gid, "from_self": False,
        })
        if self.has_file(fid):
            return
        if fid in self._downloading or fid in self._active_downloads:
            return
        self._download_queue.put({
            "fid": fid, "name": f.get("name", "?"), "size": f.get("size", 0),
            "gid": gid, "gname": gname,
            "sender_uid": sender_uid, "sender_name": sender,
        })

    # ------------------------------------------------------------- 同步请求 --
    def _sync_req(self, gid: str):
        """加入群组时请求群文件列表：让其他有文件的人重新广播 file_pub。"""
        self._broadcast({"t": "file_sync_req", "gid": gid, "gname": self.groups.get(gid, {}).get("gname", "")})

    def _on_sync_req(self, msg: dict):
        gid = msg.get("gid")
        if not gid:
            return
        for fid, r in self.index.items():
            if gid in r.get("groups", []) and self.has_file(fid):
                self._broadcast({
                    "t": "file_pub", "gid": gid, "gname": msg.get("gname", ""),
                    "file": {"id": fid, "name": r.get("name", "?"), "size": r.get("size", 0)},
                    "ts": now_ts(), "resync": True,
                    "sender_uid": r.get("src_uid", ""), "sender_name": r.get("src_name", ""),
                })

    # ------------------------------------------------------------- 下载主线程 --
    def _download_worker(self):
        while self._running:
            try:
                task = self._download_queue.get(timeout=1.0)
            except queue.Empty:
                continue
            try:
                self._do_download(task, attempt=task.get("attempt", 0))
            except Exception:
                if self._running:
                    traceback.print_exc()
                    self._emit("file_status", {"id": task["fid"], "status": "error",
                                               "note": "下载失败，可手动重试"})
            finally:
                self._downloading.discard(task["fid"])

    def _do_download(self, task: dict, attempt: int):
        fid = task["fid"]
        self._downloading.add(fid)
        self._active_downloads[fid] = task
        self._emit("file_status", {"id": fid, "status": "downloading", "progress": 0.0,
                                   "note": "正在下载（点对点）..."})
        self._emit("syschat", task["gid"],
                   f"检测到「{task['gname']}」群「{task['sender_name']}」的新文件「{task['name']}」，开始自动下载...")

        # 1) 发送人在线 -> 点对点直连
        direct_ok = False
        if self.is_online(task.get("sender_uid")):
            p = self.peers[task["sender_uid"]]
            try:
                direct_ok = self._fetch_from(p["ip"], p["tcp_port"], fid, task)
            except Exception:
                traceback.print_exc()
            if direct_ok:
                self._finish_download(task)
                return

        # 2) 离线/失败 -> 公共询问（日志全网可见），等第一个应答
        req_id = self._new_msg_id()
        self._offers[req_id] = []
        self._public_request(task, req_id)
        deadline = now_ts() + OFFER_WAIT
        while self._running and now_ts() < deadline:
            offers = self._offers.get(req_id, [])
            if offers:
                ip, port, uid = offers[0]
                try:
                    ok = self._fetch_from(ip, port, fid, task)
                except Exception:
                    traceback.print_exc()
                    ok = False
                if ok:
                    self._finish_download(task)
                    self._offers.pop(req_id, None)
                    return
                # 第一个失效 -> 尝试下一个
                offers.pop(0)
                continue
            time.sleep(0.3)

        if attempt < FALLBACK_RETRY:
            self._emit("syschat", task["gid"],
                       f"文件「{task['name']}」暂无人可提供，稍后自动重试（{attempt + 1}/{FALLBACK_RETRY}）...")
            task["attempt"] = attempt + 1
            self._download_queue.put(task)
        else:
            self._emit("file_status", {"id": fid, "status": "error",
                                       "note": "发送人离线且无缓存应答，可稍后重试"})
            self._active_downloads.pop(fid, None)

    def _public_request(self, task: dict, req_id: str):
        """向全网公共询问文件（所有人都能在日志里看到）。"""
        self._offers[req_id] = []
        self._broadcast({
            "t": "file_req", "req_id": req_id,
            "gid": task["gid"], "gname": task["gname"],
            "sender_uid": task.get("sender_uid", ""), "sender_name": task.get("sender_name", ""),
            "file": {"id": task["fid"], "name": task["name"], "size": task["size"]},
            "ts": now_ts(),
        })
        # 本机也在日志里看到这条公共询问（请求方自己可见）
        self._emit("log", f"[{fmt_time(now_ts())}] {self.my_name}: 公共询问文件「{task['name']}」"
                          f"（发送人 {task.get('sender_name', '?')}，群「{task['gname']}」）")

    def _on_file_req(self, msg: dict, addr):
        """收到公共询问：我有该文件 -> 返回我的局域网 IP（没有则无应答）。"""
        fid = (msg.get("file") or {}).get("id")
        req_id = msg.get("req_id")
        if not fid or not req_id:
            return
        self._emit("log", f"[{fmt_time(msg.get('ts', now_ts()))}] {msg.get('name', '?')}: "
                          f"公共询问文件「{(msg.get('file') or {}).get('name', '?')}」（发送人 "
                          f"{msg.get('sender_name', '?')}，群「{msg.get('gname', '?')}」）")
        if self.has_file(fid):
            self._broadcast({
                "t": "file_have", "req_id": req_id, "file_id": fid,
                "ip": get_local_ips()[0], "port": self.my_tcp_port(),
                "ts": now_ts(),
            })

    def _on_file_have(self, msg: dict):
        req_id = msg.get("req_id")
        if req_id in self._offers:
            self._offers[req_id].append((msg.get("ip"), int(msg.get("port") or 0), msg.get("uid")))

    # ------------------------------------------------------------- 从某处下载 --
    def _fetch_from(self, ip: str, port: int, fid: str, task: dict) -> bool:
        if not ip or not port:
            return False
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(8)
        try:
            sock.connect((ip, int(port)))
            send_json(sock, {"cmd": "get", "file_id": fid})
            hdr = recv_json(sock)
            if hdr.get("cmd") != "data":
                raise RuntimeError(f"对方应答：{hdr.get('err', '未知错误')}")
            size = int(hdr.get("size", 0))
        finally:
            sock.settimeout(60)
        # 写临时文件
        tmp = self.cache_dir() / (fid + ".part")
        h = hashlib.sha256()
        done = 0
        last_emit = 0.0
        with open(tmp, "wb") as f:
            while self._running:
                blk = sock.recv(FILE_CHUNK)
                if not blk:
                    break
                f.write(blk)
                h.update(blk)
                done += len(blk)
                prog = (done / size) if size else 1.0
                if now_ts() - last_emit > 0.2 or prog >= 1.0:
                    self._emit("file_status", {"id": fid, "status": "downloading",
                                               "progress": min(prog, 1.0), "note": "正在下载..."})
                    last_emit = now_ts()
        sock.close()
        if done != size:
            raise RuntimeError(f"数据不完整：期望 {size}，实际 {done}")
        if h.hexdigest() != fid:
            raise RuntimeError("文件校验失败（指纹不一致）")
        cache_final = self.cache_dir() / fid
        if cache_final.exists():
            cache_final.unlink()
        tmp.rename(cache_final)
        return True

    def _finish_download(self, task: dict):
        """下载完毕：登记索引 + 拷贝到群下载目录（友好文件名） + 系统通知。"""
        fid = task["fid"]
        cache = self.cache_dir() / fid
        gdir = self.downloads_dir(task["gid"], task["gname"])
        name = sanitize_name(task.get("name") or "file")
        target = gdir / name
        if target.exists() and target.stat().st_size != task.get("size"):
            stem, suffix = os.path.splitext(name)
            n = 1
            while target.exists():
                target = gdir / f"{stem} ({n}){suffix}"
                n += 1
        import shutil
        shutil.copy2(cache, target)
        self._register_file(fid, {
            "name": name, "size": task.get("size", 0), "ts": now_ts(),
            "src_uid": task.get("sender_uid"), "src_name": task.get("sender_name"),
            "groups": [task["gid"]], "has_local": True,
            "local_path": str(target),
        })
        self._active_downloads.pop(fid, None)
        # 系统 API 通知：按你要求的格式
        title = "文件下载完成"
        body = f"「{task['gname']}」群「{task.get('sender_name', '?')}」的「{task['name']}」下载完毕"
        self._emit("notify", title, body)
        self._emit("file_status", {"id": fid, "status": "done", "path": str(target),
                                   "note": "已下载"})
        self._emit("syschat", task["gid"], f"文件「{task['name']}」下载完毕：{target}")
        self.broadcast_log(f"「{self.my_name}」下载了「{task['gname']}」群「{task.get('sender_name', '?')}」"
                           f"的文件「{task['name']}」")

    # ------------------------------------------------------------- TCP 服务 --
    def _tcp_loop(self):
        while self._running:
            try:
                conn, addr = self._tcp_srv.accept()
            except OSError:
                break
            t = threading.Thread(target=self._safe(lambda: self._handle_conn(conn, addr)),
                                 name="tcp-conn", daemon=True)
            t.start()

    def _handle_conn(self, conn: socket.socket, addr):
        conn.settimeout(30)
        try:
            req = recv_json(conn)
            if req.get("cmd") == "get":
                fid = str(req.get("file_id") or "")
                cache = self.cache_dir() / fid
                if fid and cache.is_file():
                    send_json(conn, {"cmd": "data", "file_id": fid,
                                     "name": self.index.get(fid, {}).get("name", ""),
                                     "size": cache.stat().st_size})
                    with open(cache, "rb") as f:
                        while True:
                            blk = f.read(FILE_CHUNK)
                            if not blk:
                                break
                            conn.sendall(blk)
                else:
                    send_json(conn, {"cmd": "err", "err": "没有该文件"})
            elif req.get("cmd") == "secret_store":
                self._on_secret_store(req)
                send_json(conn, {"cmd": "secret_stored", "store_id": req.get("store_id", "")})
            elif req.get("cmd") == "secret":
                ok = self._on_secret_deliver(req)
                send_json(conn, {"cmd": "secret_ack",
                                 "store_id": req.get("store_id", ""), "ok": ok})
            else:
                send_json(conn, {"cmd": "err", "err": "未知命令"})
        except Exception:
            pass
        finally:
            try:
                conn.close()
            except Exception:
                pass

    # ------------------------------------------------------------- 检查更新 --
    def check_update(self, manual: bool = False):
        """联网时校验 update.wenyi 清单：未联网/无更新 -> 无任何反馈；
        版本号不同 -> 弹窗提醒（版本号 + 更新内容），可选择克隆更新。"""
        def work():
            try:
                url = str(self.cfg.get("update_url") or "").strip()
                if not url:
                    self._emit("update", {"ok": False, "reason": "no-url"})
                    return
                req = urllib.request.Request(
                    url, headers={"User-Agent": f"MCTierLAN/{APP_VERSION}"})
                with urllib.request.urlopen(req, timeout=8) as r:
                    data = r.read(65536).decode("utf-8", "replace")
                manifest = parse_update_manifest(data)
                remote = manifest["version"]
                notes = manifest["notes"]
                if remote and remote != APP_VERSION:
                    self._emit("update", {"ok": True, "found": True, "remote": remote,
                                          "notes": notes, "local": APP_VERSION,
                                          "page": self.cfg.get("update_page", ""),
                                          "repo": self.cfg.get("update_repo", "")})
                else:
                    self._emit("update", {"ok": True, "found": False})
            except Exception:
                # 未联网 / 服务器无响应 -> 静默，无反馈
                self._emit("update", {"ok": False, "reason": "offline"})
        t = threading.Thread(target=work, name="update-check", daemon=True)
        t.start()

    # ------------------------------------------------------------- 克隆更新 --
    def clone_update(self, repo_url: str = "") -> dict:
        """克隆更新（更新方式：克隆）：

        1) 把当前 data/ 目录（用户所有会话内容：群组、聊天、文件索引、转存等）
           完整备份到程序目录的新文件夹 customer_god/；
        2) 用 git clone 把 GitHub 上的最新版本克隆到程序目录 update_clone/；
        3) 把 customer_god/ 里的会话内容放回新版本的 data/ 目录。

        返回 {"ok": bool, "path": str, "backup": str, "reason": str}
        """
        root = update_root()
        repo_url = str(repo_url or self.cfg.get("update_repo") or "").strip()
        if not repo_url or "<" in repo_url:
            return {"ok": False,
                    "reason": "还没有配置更新仓库地址：请把 config.json 里的 "
                              "update_repo 改成你自己的 GitHub 仓库地址"}
        backup = root / CUSTOMER_DATA_DIR
        target = root / UPDATE_CLONE_DIR
        try:
            # 1) 备份「用户所有会话内容」
            self._emit("update_progress", f"正在把会话内容备份到 {CUSTOMER_DATA_DIR}/ ...")
            backup.mkdir(parents=True, exist_ok=True)
            shutil.copytree(self.data_root, backup, dirs_exist_ok=True)
            self._emit("update_progress", f"会话内容已备份：{backup}")

            # 2) 克隆最新代码
            self._emit("update_progress", "正在从 GitHub 克隆最新版本（视网速而定）...")
            if target.exists():
                shutil.rmtree(target, ignore_errors=True)
            out = self._run_git(["git", "clone", "--depth", "1", repo_url, str(target)])
            if not (target / "mctier_lan.py").is_file():
                return {"ok": False, "reason": f"克隆失败或仓库内容不完整：{out.strip()[-300:]}"}

            # 3) 会话内容回填到新版本
            self._emit("update_progress", "正在把会话内容恢复到新版本 ...")
            self.restore_customer_data(backup, target)
            self._emit("update_progress", f"更新完成，新版本在：{target}")
            return {"ok": True, "path": str(target), "backup": str(backup)}
        except Exception as e:
            return {"ok": False, "reason": f"{type(e).__name__}: {e}"}

    def restore_customer_data(self, backup: Path, target: Path):
        """把 customer_god/ 里备份的会话内容复制进新版本目录的 data/。"""
        newdata = Path(target) / "data"
        newdata.mkdir(parents=True, exist_ok=True)
        for item in Path(backup).iterdir():
            dst = newdata / item.name
            if item.is_dir():
                shutil.copytree(item, dst, dirs_exist_ok=True)
            else:
                shutil.copy2(item, dst)

    def _run_git(self, cmd: list) -> str:
        """执行 git 命令（输出写入日志文件，避免管道问题），返回输出文本。"""
        logfile = self.data_root / "update_clone.log"
        try:
            with open(logfile, "w", encoding="utf-8", errors="replace") as f:
                subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=900)
        except FileNotFoundError:
            return "未找到 git 命令：请先安装 Git（https://git-scm.com/downloads）"
        except Exception as e:
            return f"{type(e).__name__}: {e}"
        try:
            return logfile.read_text(encoding="utf-8", errors="replace")
        except Exception:
            return ""


# ----------------------------------------------------------------------------
# GUI：仿 Windows 11 / 微信 4.x 桌面版（Tkinter 原生绘制）
#   布局三栏：左侧窄图标栏 | 中间会话(群组)列表 | 右侧聊天主区 + 消息气泡
# ----------------------------------------------------------------------------
FAMILY = "Microsoft YaHei UI" if OS_IS_WINDOWS else "DejaVu Sans"
EMOJI  = "Segoe UI Emoji" if OS_IS_WINDOWS else "DejaVu Sans"


class P:
    """取色板：微信 4.x 亮色主题。"""
    bg          = "#f7f7f7"
    panel       = "#ffffff"
    hover       = "#f2f2f2"
    selected    = "#e8f7ee"
    accent      = "#07c160"
    accent_hv   = "#06ad56"
    text        = "#191919"
    sub         = "#9a9a9a"
    border      = "#e6e6e6"
    bubble_self = "#95ec69"
    bubble_other = "#ffffff"
    badge       = "#f76260"
    log_bg      = "#f5f8fc"
    entry_bg    = "#f3f3f3"


AVATAR_COLORS = ["#07c160", "#576b95", "#f76260", "#10aeff", "#ffc300",
                 "#8a2be2", "#ff9f40", "#00b0b9", "#e062a4", "#5f9ea0"]


def avatar_color(name: str) -> str:
    return AVATAR_COLORS[sum(ord(c) for c in (name or "?")) % len(AVATAR_COLORS)]


class Avatar(tk.Canvas):
    """圆形头像（纯 Canvas 绘制，无需图片资源）。"""

    def __init__(self, master, name: str, size: int = 36, bg=None, **kw):
        parent_bg = bg if bg else str(master.cget("bg"))
        super().__init__(master, width=size, height=size, bg=parent_bg,
                         highlightthickness=0, bd=0, **kw)
        color = avatar_color(name or "?")
        self.create_oval(1, 1, size - 1, size - 1, fill=color, outline=color)
        self.create_text(size / 2, size / 2, text=(name or "?")[:1],
                         fill="#ffffff", font=(FAMILY, max(9, int(size * 0.38)), "bold"))


class ScrollFrame(tk.Frame):
    """带滚动条的纵向容器（微信式滚动列表）。"""

    def __init__(self, master, bg, mwheel=True, **kw):
        super().__init__(master, bg=bg, **kw)
        self.bg = bg
        self.canvas = tk.Canvas(self, bg=bg, highlightthickness=0, bd=0)
        self.vsb = tk.Scrollbar(self, orient="vertical", width=8,
                                command=self.canvas.yview)
        self.inner = tk.Frame(self.canvas, bg=bg)
        self.win = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.canvas.configure(yscrollcommand=self.vsb.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.vsb.pack(side="right", fill="y")
        self.inner.bind("<Configure>", lambda e: self.canvas.configure(
            scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>", lambda e: self.canvas.itemconfigure(
            self.win, width=e.width))
        if mwheel:
            self.canvas.bind("<Enter>", lambda e: self.canvas.bind_all(
                "<MouseWheel>", self._on_wheel))
            self.canvas.bind("<Leave>", lambda e: self.canvas.unbind_all(
                "<MouseWheel>"))

    def _on_wheel(self, e):
        self.canvas.yview_scroll(int(-e.delta / 120), "units")

    def scroll_bottom(self):
        self.update_idletasks()
        self.canvas.yview_moveto(1.0)


def _bind_click(widget, fn):
    """给控件及其所有子控件绑定单击。"""
    widget.bind("<Button-1>", fn)
    for c in widget.winfo_children():
        _bind_click(c, fn)


class GroupCard(tk.Frame):
    """微信式的群组会话卡片。"""

    def __init__(self, master, app, gid: str, gname: str):
        super().__init__(master, bg=P.panel, cursor="hand2")
        self.app = app
        self.gid = gid
        self._bg = P.panel
        self.avatar = Avatar(self, gname, 40, bg=P.panel)
        self.avatar.pack(side="left", padx=(10, 8), pady=9)
        self._paintw = [self, self.avatar]

        mid = tk.Frame(self, bg=P.panel)
        mid.pack(side="left", fill="both", expand=True)
        self._paintw.append(mid)
        tk.Label(mid, text=gname, bg=P.panel, fg=P.text,
                 font=(FAMILY, 11, "bold"), anchor="w").pack(fill="x", pady=(7, 0))
        self.sub = tk.Label(mid, text="点击进入群聊", bg=P.panel, fg=P.sub,
                            font=(FAMILY, 9), anchor="w")
        self.sub.pack(fill="x", pady=(1, 7))
        self._paintw.extend([mid, self.sub])

        right = tk.Frame(self, bg=P.panel)
        right.pack(side="right", padx=12)
        self._paintw.append(right)
        self.time_lbl = tk.Label(right, text="", bg=P.panel, fg=P.sub, font=(FAMILY, 9))
        self.time_lbl.pack(anchor="e", pady=(8, 0))
        self.badge = tk.Label(right, text="", bg=P.badge, fg="#ffffff",
                              font=(FAMILY, 8, "bold"), padx=5, pady=1)
        self._paintw.extend([right, self.time_lbl])
        self.selected = False
        _bind_click(self, lambda e, g=gid: app.select_gid(g))
        self.bind("<Button-3>", self._ctx)

    def _ctx(self, e):
        self.app.show_group_menu(self.gid, e.x_root, e.y_root)

    def set_selected(self, sel: bool):
        self.selected = sel
        self._apply_bg(P.selected if sel else P.panel)

    def set_hover(self, on: bool):
        if not self.selected:
            self._apply_bg(P.hover if on else P.panel)

    def _apply_bg(self, color: str):
        self._bg = color
        for w in self._paintw:
            try:
                w.configure(bg=color)
            except Exception:
                pass

    def set_preview(self, text: str, ts: float = None):
        self.sub.configure(text=str(text)[:28] or "…")
        if ts:
            self.time_lbl.configure(text=fmt_time(ts)[5:])

    def set_badge(self, n: int):
        if n > 0:
            self.badge.configure(text=str(n) if n < 100 else "99+")
            self.badge.pack(anchor="e", pady=(4, 0))
        else:
            self.badge.pack_forget()


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.events: queue.Queue = queue.Queue()
        self.cfg = load_config()
        self.core = PeerCore(self.cfg, self.events, name_getter=lambda: self.cfg["name"])
        self.notifier = SystemNotifier(root)

        self.current_gid = None
        self.chat_cache = {}      # gid -> [{"t":"chat"|"sys", ...}]
        self.file_model = {}      # gid -> {file_id: {...}}
        self.unread = {}          # gid -> int
        self.card_widgets = {}    # gid -> GroupCard
        self._rendered_n = 0      # 已渲染消息条数（增量绘制）
        self._chip_cur = "chat"
        self._update_shown = False
        self._manually_check = False
        self._updating = False      # 是否正在克隆更新
        self._nav = "chat"
        self._settings_dlg = None   # 当前打开的设置对话框（投票进度/结果转发给它）
        self.dm_windows = {}        # uid -> ChatWindow（双人私聊窗口）

        self._build_ui()
        self.core.start()
        self.core.check_update(manual=False)   # 每次启动自动检查

        self._refresh_groups_list()
        self._refresh_peers()
        self._rebuild_file_model_from_index()
        self.root.after(100, self._poll_events)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---------------------------------------------------------------- UI 构建 --
    def _build_ui(self):
        self.root.title(f"{APP_NAME} v{APP_VERSION}  ·  局域网群组")
        self.root.geometry("1180x740")
        self.root.minsize(980, 620)
        self.root.configure(bg=P.bg)
        self.root.option_add("*Font", (FAMILY, 10))
        self._apply_win11_rounding()

        shell = tk.Frame(self.root, bg=P.bg)
        shell.pack(fill="both", expand=True)

        # ================= 第一栏：窄图标导航栏（微信 4.x 左侧） =================
        rail = tk.Frame(shell, width=64, bg=P.panel)
        rail.pack(side="left", fill="y")
        rail.pack_propagate(False)
        tk.Frame(rail, width=1, bg=P.border).pack(side="right", fill="y")

        logo = tk.Label(rail, text="群", bg=P.accent, fg="#ffffff",
                        font=(FAMILY, 15, "bold"), width=2, pady=4)
        logo.pack(pady=(14, 12))
        self.rail_btns = {}
        for key, icon, label in (("chat", "💬", "聊天"), ("log", "📋", "日志")):
            b = tk.Frame(rail, bg=P.panel, cursor="hand2")
            tk.Label(b, text=icon, bg=P.panel, font=(EMOJI, 16)).pack()
            tk.Label(b, text=label, bg=P.panel, fg=P.sub, font=(FAMILY, 8)).pack()
            b.pack(pady=3, ipadx=2)
            _bind_click(b, lambda e, k=key: self.switch_nav(k))
            self.rail_btns[key] = b
        self._rail_pad = tk.Frame(rail, bg=P.panel)
        self._rail_pad.pack(fill="both", expand=True)

        # 底部：我的头像（改名）+ 设置
        self.my_avatar = Avatar(rail, self.cfg["name"], 36, bg=P.panel)
        self.my_avatar.pack(pady=(0, 2))
        _bind_click(self.my_avatar, lambda e: self._open_settings())
        self.my_name_lbl = tk.Label(rail, text=self.cfg["name"][:4], bg=P.panel,
                                    fg=P.sub, font=(FAMILY, 8))
        self.my_name_lbl.pack(pady=(0, 6), padx=2)
        set_btn = tk.Frame(rail, bg=P.panel, cursor="hand2")
        tk.Label(set_btn, text="⚙", bg=P.panel, fg=P.sub, font=(FAMILY, 13)).pack()
        tk.Label(set_btn, text="设置", bg=P.panel, fg=P.sub, font=(FAMILY, 8)).pack()
        set_btn.pack(pady=(0, 12))
        _bind_click(set_btn, lambda e: self._open_settings())

        # ================= 第二栏：会话（群组）列表 =================
        listp = tk.Frame(shell, width=300, bg=P.panel)
        listp.pack(side="left", fill="y")
        listp.pack_propagate(False)
        tk.Frame(listp, width=1, bg=P.border).pack(side="right", fill="y")

        head = tk.Frame(listp, bg=P.panel)
        head.pack(fill="x", padx=14, pady=(16, 8))
        tk.Label(head, text="群聊", bg=P.panel, fg=P.text,
                 font=(FAMILY, 15, "bold")).pack(side="left")

        # 【创建群组】主按钮（醒目绿色）+ 加入群组
        btns = tk.Frame(listp, bg=P.panel)
        btns.pack(fill="x", padx=12, pady=(0, 8))
        self.create_btn = tk.Button(btns, text="＋ 创建群组", bg=P.accent, fg="#ffffff",
                                    activebackground=P.accent_hv, activeforeground="#ffffff",
                                    relief="flat", bd=0, cursor="hand2",
                                    font=(FAMILY, 10, "bold"), pady=4,
                                    command=self._on_create_group)
        self.create_btn.pack(side="left", fill="x", expand=True, padx=(0, 6))
        self.join_btn = tk.Button(btns, text="加入群组", bg=P.panel, fg=P.accent,
                                  activebackground=P.selected, activeforeground=P.accent,
                                  relief="solid", bd=1, cursor="hand2", width=9,
                                  font=(FAMILY, 10), pady=3,
                                  command=self._on_join_group)
        self.join_btn.pack(side="left")

        # 搜索框
        self.search_var = tk.StringVar()
        self.search_ph = True
        self.search_entry = tk.Entry(listp, textvariable=self.search_var, bg=P.entry_bg,
                                     fg=P.sub, relief="flat", bd=0, insertbackground=P.text,
                                     font=(FAMILY, 10), highlightthickness=0)
        self.search_entry.pack(fill="x", padx=12, pady=(2, 8), ipady=5)
        self.search_var.trace_add("write", lambda *a: self._on_search())
        self.search_entry.bind("<FocusIn>", self._on_search_focus_in)
        self.search_entry.bind("<FocusOut>", self._on_search_focus_out)
        self.search_entry.insert(0, "🔍  搜索群组")

        # 群组卡片列表
        self.groups_scroll = ScrollFrame(listp, P.panel)
        self.groups_scroll.pack(fill="both", expand=True)

        # ================= 第三栏：右侧主区 =================
        self.right_main = tk.Frame(shell, bg=P.bg)
        self.right_main.pack(side="left", fill="both", expand=True)

        # -- 群组头部 --
        self.header = tk.Frame(self.right_main, bg=P.panel, height=64)
        self.header.pack(fill="x")
        self.header.pack_propagate(False)
        tk.Frame(self.header, bg=P.border).pack(side="bottom", fill="x")

        self.hd_name = tk.Label(self.header, text="", bg=P.panel, fg=P.text,
                                font=(FAMILY, 14, "bold"))
        self.hd_name.pack(side="left", padx=(18, 0), pady=(6, 0))
        self.hd_info = tk.Label(self.header, text="", bg=P.panel, fg=P.sub,
                                font=(FAMILY, 9))
        self.hd_info.pack(side="left", padx=8, pady=(10, 0))

        # 视图切换 chips：聊天 / 群文件 / 成员
        self.chips = {}
        chips_bar = tk.Frame(self.header, bg=P.panel)
        chips_bar.pack(side="right", padx=16, pady=(10, 0))
        for key, label in (("chat", "聊天"), ("files", "群文件"), ("members", "成员")):
            ch = tk.Label(chips_bar, text=label, bg=P.panel, fg=P.sub, pady=4, padx=14,
                          cursor="hand2", font=(FAMILY, 10))
            ch.pack(side="left", padx=2)
            _bind_click(ch, lambda e, k=key: self._set_chip(k))
            self.chips[key] = ch
        # 退出群组
        self.leave_btn = tk.Button(self.header, text="退出群组", bg=P.panel, fg=P.sub,
                                   activebackground=P.hover, relief="flat", bd=0,
                                   cursor="hand2", font=(FAMILY, 9),
                                   command=self._on_leave_group)
        self.leave_btn.pack(side="right", padx=(0, 14), pady=(16, 0))

        # -- 视图容器 --
        self.viewbox = tk.Frame(self.right_main, bg=P.bg)
        self.viewbox.pack(fill="both", expand=True)
        self._build_view_empty()
        self._build_view_chat()
        self._build_view_files()
        self._build_view_members()

        # -- 状态栏 --
        self.status_bar = tk.Frame(self.right_main, bg=P.panel, height=26)
        self.status_bar.pack(fill="x", side="bottom")
        tk.Frame(self.status_bar, bg=P.border).pack(side="top", fill="x")
        self.status_lbl = tk.Label(self.status_bar, text="", bg=P.panel, fg=P.sub,
                                   font=(FAMILY, 9))
        self.status_lbl.pack(side="right", padx=12)

        # ================= 右侧：日志视图（切换自导航栏） =================
        self.right_log = tk.Frame(shell, bg=P.panel)
        log_head = tk.Frame(self.right_log, bg=P.panel, height=48)
        log_head.pack(fill="x")
        log_head.pack_propagate(False)
        tk.Label(log_head, text="局域网公共日志（所有人可见 · 只读）", bg=P.panel,
                 fg=P.text, font=(FAMILY, 13, "bold")).pack(side="left", padx=18)
        tk.Button(log_head, text="清空本机显示", bg=P.panel, fg=P.sub,
                  activebackground=P.hover, relief="flat", bd=0, cursor="hand2",
                  font=(FAMILY, 9), command=self._clear_log).pack(side="right", padx=14)
        tk.Frame(self.right_log, bg=P.border).pack(fill="x")
        self.log_text = tk.Text(self.right_log, wrap="word", state="disabled",
                                bg=P.log_bg, font=("Consolas" if OS_IS_WINDOWS else "monospace", 9),
                                relief="flat", bd=0)
        ls = tk.Scrollbar(self.right_log, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=ls.set)
        self.log_text.pack(side="left", fill="both", expand=True)
        ls.pack(side="right", fill="y")

    def _apply_win11_rounding(self):
        """Win11 原生圆角窗口（DWM）。"""
        if not OS_IS_WINDOWS:
            return
        try:
            hwnd = ctypes.windll.user32.GetParent(self.root.winfo_id())
            pref = ctypes.c_int(2)   # DWMWCP_ROUND
            ctypes.windll.dwmapi.DwmSetWindowAttribute(
                hwnd, 33, ctypes.byref(pref), ctypes.sizeof(pref))
        except Exception:
            pass

    # ------------------------------------------------ 视图控制 --
    def switch_nav(self, key: str):
        self._nav = key
        for k, b in self.rail_btns.items():
            bg = P.selected if k == key else P.panel
            for c in b.winfo_children():
                try:
                    c.configure(bg=bg)
                except Exception:
                    pass
        if key == "log":
            self.right_main.pack_forget()
            self.right_log.pack(side="left", fill="both", expand=True)
        else:
            self.right_log.pack_forget()
            self.right_main.pack(side="left", fill="both", expand=True)

    def _set_chip(self, key: str):
        self._chip_cur = key
        for k, ch in self.chips.items():
            active = (k == key)
            ch.configure(fg=P.accent if active else P.sub,
                         bg=P.selected if active else P.panel,
                         font=(FAMILY, 10, "bold") if active else (FAMILY, 10))
        self._raise_view(key)

    def _raise_view(self, key: str):
        for v in self.viewbox.winfo_children():
            v.pack_forget()
        if not self.current_gid:
            self.view_empty.pack(fill="both", expand=True)
            return
        if key == "chat":
            self.view_chat.pack(fill="both", expand=True)
            self.scroll_bottom_later()
        elif key == "files":
            self._render_files()
            self.view_files.pack(fill="both", expand=True)
        elif key == "members":
            self._render_members()
            self.view_members.pack(fill="both", expand=True)

    # ------------------------------------------------ 空状态视图 --
    def _build_view_empty(self):
        self.view_empty = tk.Frame(self.viewbox, bg=P.bg)
        inner = tk.Frame(self.view_empty, bg=P.bg)
        inner.place(relx=0.5, rely=0.42, anchor="center")
        tk.Label(inner, text="💬", bg=P.bg, font=(EMOJI, 46)).pack()
        tk.Label(inner, text="选择左侧群组开始聊天", bg=P.bg, fg=P.sub,
                 font=(FAMILY, 13)).pack(pady=(8, 4))
        tk.Label(inner, text="或创建一个群组，邀请同学加入", bg=P.bg, fg=P.sub,
                 font=(FAMILY, 10)).pack()
        b = tk.Button(inner, text="＋ 创建群组", bg=P.accent, fg="#ffffff",
                      activebackground=P.accent_hv, activeforeground="#ffffff",
                      relief="flat", bd=0, cursor="hand2",
                      font=(FAMILY, 11, "bold"), padx=22, pady=6,
                      command=self._on_create_group)
        b.pack(pady=14)

    # ------------------------------------------------ 聊天视图 --
    def _build_view_chat(self):
        self.view_chat = tk.Frame(self.viewbox, bg=P.bg)
        self.chat_scroll = ScrollFrame(self.view_chat, P.bg)
        self.chat_scroll.pack(fill="both", expand=True)
        # 气泡容器（白色卡片，像微信消息区背景）
        self.msg_bg = tk.Frame(self.chat_scroll.inner, bg=P.bg)
        self.msg_bg.pack(fill="both", expand=True)

        inp = tk.Frame(self.view_chat, bg=P.panel)
        inp.pack(fill="x", side="bottom")
        tk.Frame(inp, bg=P.border).pack(side="top", fill="x")
        toolbar = tk.Frame(inp, bg=P.panel)
        toolbar.pack(fill="x", padx=12, pady=(6, 2))
        self.file_btn = tk.Button(toolbar, text="📎  发送文件", bg=P.panel, fg=P.sub,
                                  activebackground=P.hover, relief="flat", bd=0,
                                  cursor="hand2", font=(FAMILY, 10),
                                  command=self._on_send_file)
        self.file_btn.pack(side="left")
        row = tk.Frame(inp, bg=P.panel)
        row.pack(fill="x", padx=12, pady=(0, 12))
        self.entry = tk.Entry(row, bg="#f7f7f7", fg=P.text, relief="flat", bd=0,
                              insertbackground=P.text, highlightthickness=0,
                              font=(FAMILY, 10))
        self.entry.pack(side="left", fill="x", expand=True, ipady=8, padx=(0, 10))
        self.entry.bind("<Return>", lambda e: self._on_send_chat())
        self.send_btn = tk.Button(row, text="发送", bg=P.accent, fg="#ffffff",
                                  activebackground=P.accent_hv, activeforeground="#ffffff",
                                  relief="flat", bd=0, cursor="hand2",
                                  font=(FAMILY, 10, "bold"), width=9, pady=8,
                                  command=self._on_send_chat)
        self.send_btn.pack(side="left")

    def scroll_bottom_later(self):
        self.root.after(60, lambda: self.chat_scroll.scroll_bottom())

    def _msg_kind_row(self, m: dict):
        """在消息区追加一条消息（微信气泡样式）。"""
        if m.get("t") == "sys":
            tk.Label(self.msg_bg, text=f"── {m['text']} ──", bg=P.bg, fg=P.sub,
                     font=(FAMILY, 9)).pack(pady=3, anchor="center")
            return
        mine = bool(m.get("self"))
        row = tk.Frame(self.msg_bg, bg=P.bg)
        row.pack(fill="x", padx=16, pady=5)
        side = "right" if mine else "left"
        box = tk.Frame(row, bg=P.bg)
        box.pack(side=side, anchor="e" if mine else "w")

        if mine:
            bubble = tk.Label(box, text=m["text"], bg=P.bubble_self, fg=P.text,
                              wraplength=430, justify="left", padx=12, pady=8,
                              font=(FAMILY, 10))
            bubble.pack(anchor="e")
            tk.Label(box, text=f"{fmt_time(m['ts'])[5:]}  我", bg=P.bg, fg=P.sub,
                     font=(FAMILY, 8)).pack(anchor="e", pady=(2, 0))
        else:
            top = tk.Frame(box, bg=P.bg)
            top.pack(anchor="w")
            av = Avatar(top, str(m.get("from", "?")), 30, bg=P.bg)
            av.pack(side="left", padx=(0, 6))
            nm = tk.Frame(top, bg=P.bg)
            nm.pack(side="left")
            name_lbl = tk.Label(nm, text=str(m.get("from", "?")), bg=P.bg, fg=P.sub,
                                font=(FAMILY, 8, "bold"))
            name_lbl.pack(anchor="w")
            tk.Label(nm, text=fmt_time(m["ts"])[5:], bg=P.bg, fg=P.sub,
                     font=(FAMILY, 8)).pack(anchor="w")
            # 点对方头像 / 名字 -> 快速私聊
            fuid = m.get("from_uid")
            if fuid:
                av.configure(cursor="hand2")
                name_lbl.configure(cursor="hand2")
                av.bind("<Button-1>", lambda e, u=fuid: self.open_dm(u))
                name_lbl.bind("<Button-1>", lambda e, u=fuid: self.open_dm(u))
            bubble = tk.Label(box, text=m["text"], bg=P.bubble_other, fg=P.text,
                              wraplength=430, justify="left", padx=12, pady=8,
                              font=(FAMILY, 10))
            bubble.pack(anchor="w", padx=(0, 40), pady=(2, 0))

    # ------------------------------------------------ 群文件视图 --
    def _build_view_files(self):
        self.view_files = tk.Frame(self.viewbox, bg=P.bg)
        style = ttk.Style(self.root)
        try:
            style.configure("Files.Treeview", rowheight=34, font=(FAMILY, 10),
                            background=P.panel, fieldbackground=P.panel)
            style.configure("Files.Treeview.Heading", font=(FAMILY, 10, "bold"),
                            background=P.panel, foreground=P.sub)
            style.map("Files.Treeview", background=[("selected", P.selected)],
                      foreground=[("selected", P.text)])
        except Exception:
            pass
        cols = ("name", "size", "sender", "status")
        self.files_tree = ttk.Treeview(self.view_files, columns=cols, show="headings",
                                       style="Files.Treeview")
        for c, t, w in (("name", "文件名", 320), ("size", "大小", 100),
                        ("sender", "发送人", 130), ("status", "状态", 160)):
            self.files_tree.heading(c, text=t)
            self.files_tree.column(c, width=w, anchor="w")
        fs = tk.Scrollbar(self.view_files, command=self.files_tree.yview)
        self.files_tree.configure(yscrollcommand=fs.set)
        self.files_tree.pack(side="left", fill="both", expand=True, padx=(14, 0), pady=10)
        fs.pack(side="left", fill="y", pady=10)
        self.files_tree.bind("<Double-1>", lambda e: self._on_open_selected_file())

        fbar = tk.Frame(self.view_files, bg=P.bg)
        fbar.pack(side="top", fill="x", padx=14, pady=(10, 0), anchor="w")
        for text, cmd in (("打开所选文件", self._on_open_selected_file),
                          ("重试下载", self._on_retry_download),
                          ("打开下载目录", self._on_open_downloads)):
            tk.Button(fbar, text=text, bg=P.panel, fg=P.sub, activebackground=P.hover,
                      relief="solid", bd=1, cursor="hand2", font=(FAMILY, 9),
                      command=cmd).pack(side="left", padx=(0, 8))

    # ------------------------------------------------ 成员视图 --
    def _build_view_members(self):
        self.view_members = tk.Frame(self.viewbox, bg=P.bg)
        self.members_scroll = ScrollFrame(self.view_members, P.bg)
        self.members_scroll.pack(fill="both", expand=True, padx=14, pady=10)
        self.members_inner = self.members_scroll.inner
        self.members_count_lbl = tk.Label(self.members_inner, text="", bg=P.bg,
                                          fg=P.sub, font=(FAMILY, 9))
        self.members_count_lbl.pack(anchor="w", pady=(0, 6))

    def _render_members(self):
        for w in self.members_inner.winfo_children():
            if w is not self.members_count_lbl:
                w.destroy()
        gid = self.current_gid
        if not gid:
            return
        members = self.core.group_members(gid)
        self.members_count_lbl.configure(text=f"本群在线成员 · {len(members)} 人"
                                              f" · 点击成员头像可私聊")
        for m in members:
            card = tk.Frame(self.members_inner, bg=P.panel,
                            cursor="hand2" if not m.get("self") else "")
            card.pack(fill="x", pady=3)
            Avatar(card, m["name"], 36, bg=P.panel).pack(side="left", padx=(10, 8))
            box = tk.Frame(card, bg=P.panel)
            box.pack(side="left", fill="x", expand=True, pady=7)
            tk.Label(box, text=m["name"] + ("（我）" if m.get("self") else ""),
                     bg=P.panel, fg=P.text, font=(FAMILY, 10, "bold"),
                     anchor="w").pack(fill="x")
            tk.Label(box, text=f"局域网地址 {m['ip']}", bg=P.panel, fg=P.sub,
                     font=(FAMILY, 8), anchor="w").pack(fill="x")
            if m.get("self"):
                tk.Label(card, text="我自己", bg=P.panel, fg=P.sub,
                         font=(FAMILY, 8)).pack(side="right", padx=10)
            else:
                tk.Label(card, text="私聊 →", bg=P.panel, fg=P.accent,
                         font=(FAMILY, 9)).pack(side="right", padx=10)
                _bind_click(card, lambda e, uid=m["uid"]: self.open_dm(uid))
            tk.Frame(card, height=1, bg=P.border).pack(side="bottom", fill="x")

    # ------------------------------------------------ 群组列表刷新 --
    def _refresh_groups_list(self):
        for w in self.card_widgets.values():
            w.destroy()
        self.card_widgets = {}
        q = self.search_var.get().strip().lower() if not self.search_ph else ""
        for g in self.core.my_groups():
            gid, gname = g["gid"], g["gname"]
            if q and q not in gname.lower() and q not in gid.lower():
                continue
            card = GroupCard(self.groups_scroll.inner, self, gid, gname)
            card.pack(fill="x", padx=8, pady=2)
            card.set_selected(gid == self.current_gid)
            card.set_badge(self.unread.get(gid, 0))
            # 预览最近一条消息
            lines = self.chat_cache.get(gid, [])
            for m in reversed(lines):
                if m.get("t") == "chat":
                    card.set_preview(f"{m.get('from', '?')}: {m['text']}", m.get("ts"))
                    break
            self.card_widgets[gid] = card
        if self.current_gid not in self.core.groups:
            self.current_gid = None
        self._update_group_header()
        self._raise_view(self._chip_cur)

    def _on_search(self):
        if self.search_ph:
            return
        self._refresh_groups_list()

    def _on_search_focus_in(self, _e=None):
        if self.search_ph:
            self.search_ph = False
            self.search_entry.delete(0, tk.END)
            self.search_entry.configure(fg=P.text)

    def _on_search_focus_out(self, _e=None):
        if not self.search_var.get().strip():
            self.search_ph = True
            self.search_entry.delete(0, tk.END)
            self.search_entry.insert(0, "🔍  搜索群组")
            self.search_entry.configure(fg=P.sub)
            self._refresh_groups_list()

    def show_group_menu(self, gid: str, x: int, y: int):
        menu = tk.Menu(self.root, tearoff=0, font=(FAMILY, 10))
        g = self.core.groups.get(gid)
        if g:
            menu.add_command(label=f"群：{g['gname']}（{gid}）", state="disabled")
            menu.add_separator()
            menu.add_command(label="退出该群组", command=lambda: self._leave_group_by(gid))
        try:
            menu.tk_popup(x, y)
        finally:
            menu.grab_release()

    def _leave_group_by(self, gid: str):
        g = self.core.groups.get(gid)
        if not g:
            return
        if messagebox.askyesno("退出群组",
                               f"确定退出群「{g['gname']}」吗？\n（已下载的文件会保留在本机）",
                               parent=self.root):
            self.core.leave_group(gid)
            if self.current_gid == gid:
                self.current_gid = None
                self._render_chat()
                self._render_files()
            self._refresh_groups_list()
            self._refresh_peers()

    # ---------------------------------------------------------------- 事件轮询 --
    def _poll_events(self):
        try:
            while True:
                ev = self.events.get_nowait()
                kind = ev[0]
                if kind == "chat":
                    self._on_ev_chat(ev[1])
                elif kind == "log":
                    self._append_log(ev[1])
                elif kind == "syslog":
                    self._append_log(f"[{fmt_time(now_ts())}] 系统: {ev[1]}")
                elif kind == "syschat":
                    self._on_syschat(ev[1], ev[2])
                elif kind == "peers_changed":
                    self._refresh_peers()
                elif kind == "groups_changed":
                    self._refresh_groups_list()
                elif kind == "name_changed":
                    self.my_name_lbl.configure(text=ev[1][:4])
                    self._redraw_my_avatar()
                elif kind == "file_new":
                    self._on_ev_file_new(ev[1])
                elif kind == "file_status":
                    self._on_ev_file_status(ev[1])
                elif kind == "notify":
                    self.notifier.notify(ev[1], ev[2])
                elif kind == "update":
                    self._on_update_result(ev[1])
                elif kind == "update_progress":
                    self._append_log(f"[{fmt_time(now_ts())}] 系统: {ev[1]}")
                elif kind == "update_done":
                    self._on_update_done(ev[1])
                elif kind == "rename_request":
                    self._on_rename_request(ev[1])
                elif kind == "rename_vote_progress":
                    dlg = self._settings_dlg
                    if dlg is not None:
                        dlg.on_vote_progress(ev[1], ev[2], ev[3])
                elif kind == "rename_vote_done":
                    dlg = self._settings_dlg
                    if dlg is not None:
                        dlg.on_vote_done(ev[1], ev[2], ev[3], ev[4])
                elif kind == "dm":
                    self._on_ev_dm(ev[1])
                elif kind == "dm_sent":
                    self._on_ev_dm_sent(ev[1])
        except queue.Empty:
            pass
        self.root.after(100, self._poll_events)

    def _redraw_my_avatar(self):
        try:
            self.my_avatar.destroy()
            self.my_avatar = Avatar(self.my_avatar.master, self.cfg["name"], 36,
                                    bg=P.panel)
            self.my_avatar.pack(pady=(14, 2))
            _bind_click(self.my_avatar, lambda e: self._open_settings())
        except Exception:
            pass

    # ---------------------------------------------------------------- 群组消息 --
    def _on_ev_chat(self, msg):
        gid = msg["gid"]
        self.chat_cache.setdefault(gid, []).append({
            "t": "chat", "from": msg["from_name"], "text": msg["text"],
            "ts": msg["ts"], "self": bool(msg.get("self")),
            "from_uid": msg.get("from_uid")})
        if len(self.chat_cache[gid]) > 2000:
            self.chat_cache[gid] = self.chat_cache[gid][-2000:]
        card = self.card_widgets.get(gid)
        if card:
            card.set_preview(f"{msg['from_name']}: {msg['text']}", msg["ts"])
        if gid == self.current_gid and self._nav == "chat":
            self._render_chat(append=True)
        if not msg.get("self"):
            if gid != self.current_gid or self._nav != "chat":
                self.unread[gid] = self.unread.get(gid, 0) + 1
                if card:
                    card.set_badge(self.unread[gid])

    def _on_syschat(self, gid, text):
        self.chat_cache.setdefault(gid, []).append({
            "t": "sys", "text": text, "ts": now_ts()})
        if len(self.chat_cache[gid]) > 2000:
            self.chat_cache[gid] = self.chat_cache[gid][-2000:]
        if gid == self.current_gid and self._nav == "chat":
            self._render_chat(append=True)

    def _render_chat(self, append=False):
        gid = self.current_gid
        if not append:
            for w in self.msg_bg.winfo_children():
                w.destroy()
            self._rendered_n = 0
        lines = self.chat_cache.get(gid, [])
        for m in lines[self._rendered_n:]:
            self._msg_kind_row(m)
        self._rendered_n = len(lines)
        if append:
            self.scroll_bottom_later()

    # ---------------------------------------------------------------- 群文件消息 --
    def _on_ev_file_new(self, info):
        gid = info["gid"]
        f = info["file"]
        m = self.file_model.setdefault(gid, {})
        fid = f["id"]
        if fid not in m:
            m[fid] = {"id": fid, "name": f["name"], "size": f["size"],
                      "sender": f["src_name"], "status": "已存在" if f.get("has_local") else "等待下载",
                      "progress": 1.0 if f.get("has_local") else 0.0, "path": ""}
        if gid == self.current_gid and self._chip_cur == "files":
            self._render_files()

    def _on_ev_file_status(self, st):
        fid = st["id"]
        for gid, m in self.file_model.items():
            if fid in m:
                row = m[fid]
                if st["status"] == "downloading":
                    row["status"] = f"下载中 {int(st.get('progress', 0) * 100)}%"
                elif st["status"] == "done":
                    row["status"] = "已下载"
                    row["path"] = st.get("path", "")
                    row["progress"] = 1.0
                    self._rebuild_file_model_from_index()
                elif st["status"] == "error":
                    row["status"] = "失败：" + st.get("note", "")
                if gid == self.current_gid and self._chip_cur == "files":
                    self._render_files()

    # ---------------------------------------------------------------- 改名投票 --
    def _on_rename_request(self, info: dict):
        if info.get("from_uid") == self.core.uid:
            return   # 自己的提案由设置对话框显示进度，不再弹窗
        try:
            RenameVoteDialog(self, info)
        except Exception:
            traceback.print_exc()

    # ---------------------------------------------------------------- 双人私聊 --
    def open_dm(self, uid: str):
        """打开（或前置）与某用户的私聊窗口。uid 无效/自己时返回 None。"""
        uid = str(uid or "")
        if not uid or uid == self.core.uid:
            return None
        w = self.dm_windows.get(uid)
        if w is None:
            p = self.core.peers.get(uid, {})
            name = p.get("name") or f"用户{uid[:6]}"
            w = ChatWindow(self, uid, name)
            self.dm_windows[uid] = w
        else:
            try:
                w.deiconify()
                w.lift()
                w.focus_set()
            except Exception:
                pass
        return w

    def _on_ev_dm(self, m: dict):
        if m.get("self"):
            uid = m.get("to_uid")
        else:
            uid = m.get("from_uid")
        if not uid:
            return
        w = self.dm_windows.get(uid)
        if w is None:
            if m.get("self"):
                return   # 自己发消息时窗口必然已打开
            w = self.open_dm(uid)
            if w is None:
                return
        w.append_msg(m)
        if not m.get("self") and not w.focused():
            w.unread += 1
            w.title(w._title())
            self.notifier.notify("私聊消息",
                                 f"「{m.get('from_name', '?')}」：{str(m.get('text', ''))[:60]}")

    def _on_ev_dm_sent(self, info: dict):
        """离线转存结果：在私聊窗口里显示转存去向/失败原因，并回显自己发出的消息。"""
        w = self.dm_windows.get(info.get("to_uid"))
        if w is None:
            return
        if info.get("ok"):
            w.append_msg({"t": "sys", "text": f"对方不在线，已加密转存给在线同学"
                          f"「{info.get('relay_name', '?')}」，待对方上线后自动送达"})
            w.append_msg({"t": "chat", "text": str(info.get("text", "")),
                          "ts": info.get("ts", now_ts()), "self": True})
        else:
            w.append_msg({"t": "sys", "text": str(info.get("reason", "转存失败"))})

    # ---------------------------------------------------------------- 更新检查 --
    def _on_update_result(self, res):
        if self._update_shown:
            return
        if res.get("ok") and res.get("found"):
            self._update_shown = True
            remote = res.get("remote", "?")
            local = res.get("local", APP_VERSION)
            notes = str(res.get("notes", "") or "").strip()
            page = res.get("page") or self.cfg.get("update_page", "")
            repo = str(res.get("repo", "") or "").strip()
            msg = f"新版本 —— v{remote}\n\n"
            if notes:
                msg += f"更新内容：\n{notes}\n\n"
            msg += (f"（当前版本 v{local}）\n\n"
                    f"「是」立即克隆更新：程序自动把新版本克隆下来，\n"
                    f"你的所有会话内容会保存到 {CUSTOMER_DATA_DIR} 文件夹并接着用；\n"
                    f"「否」打开更新页面手动下载；「取消」稍后再说。")
            answer = messagebox.askyesnocancel("发现新版本", msg, parent=self.root)
            if answer is True:
                self._start_clone_update(repo)
            elif answer is False and page:
                try:
                    webbrowser.open(page)
                except Exception:
                    pass
        # 未联网/无更新：无任何反馈（静默），符合需求

    def _start_clone_update(self, repo: str):
        """开始克隆更新（后台线程，进度写到日志页）。"""
        if self._updating:
            return
        repo = str(repo or "").strip()
        if not repo or "<" in repo:
            messagebox.showinfo(
                "还没有配置更新仓库",
                "请先把 config.json 里的 update_repo 改成你自己的 GitHub 仓库地址，\n"
                "例如：https://github.com/用户名/仓库名.git",
                parent=self.root)
            return
        self._updating = True
        self.switch_nav("log")   # 切到日志页方便看进度
        self._append_log(f"[{fmt_time(now_ts())}] 系统: 开始克隆更新 ...")
        def work():
            res = self.core.clone_update(repo)
            self._emit_gui("update_done", res)
        threading.Thread(target=work, name="clone-update", daemon=True).start()

    def _on_update_done(self, res: dict):
        self._updating = False
        if res.get("ok"):
            self._append_log(f"[{fmt_time(now_ts())}] 系统: 更新完成 -> {res.get('path')}")
            messagebox.showinfo(
                "更新完成",
                f"新版本已克隆到：\n{res.get('path')}\n\n"
                f"你的所有会话内容已备份到 {CUSTOMER_DATA_DIR}/，\n"
                f"并已复制进新版本的 data/ 目录（群组、聊天、文件都还在）。\n\n"
                f"请关闭本程序，然后到上面的目录双击「启动.bat」使用新版本。",
                parent=self.root)
        else:
            self._append_log(f"[{fmt_time(now_ts())}] 系统: 更新失败：{res.get('reason')}")
            messagebox.showwarning("更新失败", str(res.get("reason", "未知错误")),
                                   parent=self.root)

    def _on_manual_update(self):
        if not self._manually_check:
            self._manually_check = True
            self.core.check_update(manual=True)

    # ---------------------------------------------------------------- 群组操作 --
    def _update_group_header(self):
        gid = self.current_gid
        if gid and gid in self.core.groups:
            g = self.core.groups[gid]
            members = self.core.group_members(gid)
            self.hd_name.configure(text=g["gname"])
            self.hd_info.configure(text=f"群号 {gid} · 在线 {len(members)} 人")
        else:
            self.hd_name.configure(text="")
            self.hd_info.configure(text="")
        self._set_chip(getattr(self, "_chip_cur", "chat"))

    def select_gid(self, gid: str):
        if gid not in self.core.groups:
            return
        self.current_gid = gid
        self.unread[gid] = 0
        for _gid, card in self.card_widgets.items():
            card.set_selected(_gid == gid)
            if _gid == gid:
                card.set_badge(0)
        self._render_chat()
        self._render_files()
        self._render_members()
        self._update_group_header()
        self._set_chip(getattr(self, "_chip_cur", "chat"))

    def _on_create_group(self):
        CreateGroupDialog(self)

    def _on_join_group(self):
        JoinDialog(self)

    def _on_leave_group(self):
        if not self.current_gid:
            return
        self._leave_group_by(self.current_gid)

    def _on_rename(self):
        self._open_settings()

    def _open_settings(self):
        SettingsDialog(self)

    # ---------------------------------------------------------------- 聊天操作 --
    def _on_send_chat(self):
        text = self.entry.get().strip()
        if not text:
            return
        gid = self.current_gid
        if not gid:
            messagebox.showinfo("提示", "请先选择或创建一个群组", parent=self.root)
            return
        self.core.send_chat(gid, text)
        self.entry.delete(0, tk.END)

    # ---------------------------------------------------------------- 文件操作 --
    def _rebuild_file_model_from_index(self):
        for g in self.core.my_groups():
            gid = g["gid"]
            m = self.file_model.setdefault(gid, {})
            for f in self.core.files_of_group(gid):
                m.setdefault(f["id"], {"id": f["id"], "name": f["name"], "size": f["size"],
                                       "sender": f["src_name"],
                                       "status": "已下载" if f["has_local"] else "等待下载",
                                       "progress": 1.0 if f["has_local"] else 0.0,
                                       "path": ""})
        if self.current_gid and getattr(self, "_chip_cur", "chat") == "files":
            self._render_files()

    def _render_files(self):
        self.files_tree.delete(*self.files_tree.get_children())
        gid = self.current_gid
        if not gid:
            return
        m = self.file_model.get(gid, {})
        rows = sorted(m.values(), key=lambda r: (r["status"] == "已下载", -r.get("size", 0)))
        for r in rows:
            self.files_tree.insert("", tk.END, iid=r["id"],
                                   values=(r["name"], human_size(r["size"]), r["sender"], r["status"]))

    def _on_send_file(self):
        gid = self.current_gid
        if not gid:
            messagebox.showinfo("提示", "请先选择或创建一个群组", parent=self.root)
            return
        paths = filedialog.askopenfilenames(title="选择要发送到群的文件", parent=self.root)
        if not paths:
            return
        g = self.core.groups[gid]
        ok = messagebox.askyesno("发送文件",
                                 f"把所选 {len(paths)} 个文件发送到群「{g['gname']}」？\n"
                                 "群内所有成员将自动下载。", parent=self.root)
        if not ok:
            return
        def work():
            for p in paths:
                try:
                    self.core.send_file_to_group(Path(p), gid)
                except Exception as e:
                    self._emit_gui("syschat", gid, f"发送文件 {Path(p).name} 失败：{e}")
                time.sleep(1.0)
        threading.Thread(target=work, daemon=True).start()

    def _emit_gui(self, ev, *payload):
        self.events.put((ev,) + payload)

    def _selected_file_row(self):
        gid = self.current_gid
        sel = self.files_tree.selection()
        if not gid or not sel:
            return None
        m = self.file_model.get(gid, {})
        rec = m.get(sel[0])
        return rec

    def _on_open_selected_file(self):
        rec = self._selected_file_row()
        if not rec:
            messagebox.showinfo("提示", "请先在群文件列表中选择一个文件", parent=self.root)
            return
        path = Path(rec.get("path") or "")
        if not path.is_file():
            g = self.core.groups.get(self.current_gid)
            if g:
                cand = self.core.downloads_dir(self.current_gid, g["gname"]) / sanitize_name(rec["name"])
                if cand.is_file():
                    path = cand
        if not path.is_file():
            messagebox.showinfo("提示", "该文件尚未下载到本机，正在等待自动下载完成。", parent=self.root)
            return
        if not open_with_system_app(path):
            messagebox.showerror("错误", "无法用系统默认程序打开该文件", parent=self.root)

    def _on_retry_download(self):
        rec = self._selected_file_row()
        if not rec:
            return
        gid = self.current_gid
        if not gid:
            return
        g = self.core.groups.get(gid)
        rec_idx = self.core.index_record(rec["id"])
        task = {"fid": rec["id"], "name": rec["name"], "size": rec["size"],
                "gid": gid, "gname": g["gname"] if g else "",
                "sender_uid": (rec_idx or {}).get("src_uid"), "sender_name": rec["sender"]}
        self.core._download_queue.put(task)

    def _on_open_downloads(self):
        try:
            (DATA_DIR / "downloads").mkdir(parents=True, exist_ok=True)
            os.startfile(str(DATA_DIR / "downloads")) if OS_IS_WINDOWS else \
                webbrowser.open(DATA_DIR / "downloads")
        except Exception:
            pass

    def _open_data_dir(self):
        try:
            if OS_IS_WINDOWS:
                os.startfile(str(DATA_DIR))
            else:
                webbrowser.open(DATA_DIR.as_uri())
        except Exception:
            pass

    # ---------------------------------------------------------------- 日志 --
    def _append_log(self, line: str):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", line + "\n")
        self.log_text.configure(state="disabled")
        lines = int(self.log_text.index("end-1c").split(".")[0])
        if lines > LOG_MAX_LINES:
            self.log_text.delete("1.0", f"{lines - LOG_MAX_LINES}.0")
        self.log_text.see("end")

    def _clear_log(self):
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    # ---------------------------------------------------------------- peers --
    def _refresh_peers(self):
        online = [p for p in self.core.peers.values()
                  if now_ts() - (p.get("ts") or 0) <= PRESENCE_TTL]
        self.status_lbl.configure(text=f"在线用户 {len(online)} · v{APP_VERSION} · "
                                       f"本机 {'、'.join(get_local_ips())}")
        if self.current_gid:
            self._update_group_header()
        for w in list(self.dm_windows.values()):
            try:
                w._refresh_status()
            except Exception:
                pass

    # ---------------------------------------------------------------- 关闭 --
    def _on_close(self):
        self.core.stop()
        self.root.destroy()


# ----------------------------------------------------------------------------
# 双人私聊窗口（群里点成员头像 / 聊天气泡头像打开；可多窗口并存）
# ----------------------------------------------------------------------------
class ChatWindow(tk.Toplevel):
    def __init__(self, app: App, uid: str, name: str):
        super().__init__(app.root)
        self.app = app
        self.uid = uid
        self.peer_name = name
        self.unread = 0
        self.title(self._title())
        self.geometry("440x580")
        self.configure(bg=P.panel)
        self.transient(app.root)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        # 头部：头像 + 名字 + 在线状态
        head = tk.Frame(self, bg=P.panel, height=52)
        head.pack(fill="x")
        head.pack_propagate(False)
        self.head_avatar = Avatar(head, name, 34, bg=P.panel)
        self.head_avatar.pack(side="left", padx=(12, 8), pady=9)
        box = tk.Frame(head, bg=P.panel)
        box.pack(side="left", pady=7)
        tk.Label(box, text=name, bg=P.panel, fg=P.text,
                 font=(FAMILY, 11, "bold"), anchor="w").pack(fill="x")
        self.status_lbl = tk.Label(box, text="", bg=P.panel, fg=P.sub,
                                   font=(FAMILY, 8), anchor="w")
        self.status_lbl.pack(fill="x")
        tk.Frame(self, bg=P.border).pack(fill="x")

        # 消息区（气泡样式与群聊一致）
        self.scroll = ScrollFrame(self, P.bg)
        self.scroll.pack(fill="both", expand=True)
        self.msg_bg = tk.Frame(self.scroll.inner, bg=P.bg)
        self.msg_bg.pack(fill="both", expand=True)

        # 输入区
        inp = tk.Frame(self, bg=P.panel)
        inp.pack(fill="x", side="bottom")
        tk.Frame(inp, bg=P.border).pack(side="top", fill="x")
        row = tk.Frame(inp, bg=P.panel)
        row.pack(fill="x", padx=10, pady=10)
        self.entry = tk.Entry(row, bg="#f7f7f7", fg=P.text, relief="flat", bd=0,
                              insertbackground=P.text, highlightthickness=0,
                              font=(FAMILY, 10))
        self.entry.pack(side="left", fill="x", expand=True, ipady=7, padx=(0, 8))
        self.entry.bind("<Return>", lambda e: self._send())
        tk.Button(row, text="发送", bg=P.accent, fg="#ffffff",
                  activebackground=P.accent_hv, activeforeground="#ffffff",
                  relief="flat", bd=0, cursor="hand2", font=(FAMILY, 10, "bold"),
                  width=8, pady=7, command=self._send).pack(side="left")

        self._refresh_status()
        self._center(app.root)
        self.bind("<FocusIn>", lambda e: self._mark_read())
        self.entry.focus_set()

    def _title(self) -> str:
        t = f"私聊 · {self.peer_name}"
        if self.unread:
            t += f"（{self.unread} 条新消息）"
        return t

    def _on_close(self):
        self.app.dm_windows.pop(self.uid, None)
        self.destroy()

    def _center(self, root):
        self.update_idletasks()
        w, h = 440, 580
        x = root.winfo_rootx() + (root.winfo_width() - w) // 2 + 90
        y = root.winfo_rooty() + max((root.winfo_height() - h) // 2 - 20, 0)
        self.geometry(f"+{max(x, 0)}+{max(y, 0)}")

    def _refresh_status(self):
        online = self.app.core.is_online(self.uid)
        self.status_lbl.configure(text="● 在线" if online else "○ 可能离线",
                                  fg=P.accent if online else P.sub)
        self.title(self._title())

    def _mark_read(self):
        if self.unread:
            self.unread = 0
            self.title(self._title())

    def focused(self) -> bool:
        try:
            if not self.winfo_exists():
                return False
            return self.focus_displayof() is not None
        except Exception:
            return False

    def _sys(self, text: str):
        tk.Label(self.msg_bg, text=f"── {text} ──", bg=P.bg, fg=P.sub,
                 font=(FAMILY, 9)).pack(pady=3, anchor="center")

    def append_msg(self, m: dict):
        if m.get("t") == "sys":
            self._sys(m["text"])
        else:
            mine = bool(m.get("self"))
            row = tk.Frame(self.msg_bg, bg=P.bg)
            row.pack(fill="x", padx=14, pady=4)
            side = "right" if mine else "left"
            box = tk.Frame(row, bg=P.bg)
            box.pack(side=side, anchor="e" if mine else "w")
            if mine:
                bubble = tk.Label(box, text=m["text"], bg=P.bubble_self, fg=P.text,
                                  wraplength=300, justify="left", padx=12, pady=8,
                                  font=(FAMILY, 10))
                bubble.pack(anchor="e")
                tk.Label(box, text=f"{fmt_time(m['ts'])[5:]}  我", bg=P.bg, fg=P.sub,
                         font=(FAMILY, 8)).pack(anchor="e", pady=(2, 0))
            else:
                top = tk.Frame(box, bg=P.bg)
                top.pack(anchor="w")
                Avatar(top, str(m.get("from_name", "?")), 30, bg=P.bg).pack(side="left", padx=(0, 6))
                info_txt = f"{str(m.get('from_name', '?'))}  {fmt_time(m['ts'])[5:]}"
                if m.get("relayed"):
                    info_txt += "  · 离线转存送达"
                tk.Label(top, text=info_txt, bg=P.bg, fg=P.sub, font=(FAMILY, 8)).pack(side="left")
                bubble = tk.Label(box, text=m["text"], bg=P.bubble_other, fg=P.text,
                                  wraplength=300, justify="left", padx=12, pady=8,
                                  font=(FAMILY, 10))
                bubble.pack(anchor="w", padx=(0, 30), pady=(2, 0))
        self.scroll.scroll_bottom()

    def _send(self):
        text = self.entry.get().strip()
        if not text:
            return
        if not self.app.core.is_online(self.uid):
            self._sys("对方不在线，正在寻找在线同学加密转存…")
        self.app.core.send_dm_auto(self.uid, text)
        self.entry.delete(0, tk.END)


# ----------------------------------------------------------------------------
# 创建群组对话框（微信风格）
# ----------------------------------------------------------------------------
class CreateGroupDialog(tk.Toplevel):
    def __init__(self, app: App):
        super().__init__(app.root)
        self.app = app
        self.title("创建群组")
        self.geometry("360x210")
        self.resizable(False, False)
        self.configure(bg=P.panel)
        self.grab_set()
        self.transient(app.root)
        self._center(app.root)

        body = tk.Frame(self, bg=P.panel, padx=22, pady=20)
        body.pack(fill="both", expand=True)
        tk.Label(body, text="创建群组", bg=P.panel, fg=P.text,
                 font=(FAMILY, 14, "bold")).pack(anchor="w")
        tk.Label(body, text="群组名称（如：高三（2）班）", bg=P.panel, fg=P.sub,
                 font=(FAMILY, 9)).pack(anchor="w", pady=(10, 4))
        self.name_var = tk.StringVar()
        ent = tk.Entry(body, textvariable=self.name_var, bg="#f7f7f7", fg=P.text,
                       relief="flat", bd=0, insertbackground=P.text,
                       font=(FAMILY, 11), highlightthickness=0)
        ent.pack(fill="x", ipady=7)
        ent.focus_set()
        ent.bind("<Return>", lambda e: self._ok())
        tk.Label(body, text="创建后自动生成群号，其他同学加入群组时输入该群号即可",
                 bg=P.panel, fg=P.sub, font=(FAMILY, 8), wraplength=310,
                 justify="left").pack(anchor="w", pady=(6, 14))
        btns = tk.Frame(body, bg=P.panel)
        btns.pack(fill="x")
        tk.Button(btns, text="取消", bg=P.panel, fg=P.sub, activebackground=P.hover,
                  relief="flat", bd=0, cursor="hand2", font=(FAMILY, 10),
                  command=self.destroy).pack(side="right")
        self.ok_btn = tk.Button(btns, text="创建", bg=P.accent, fg="#ffffff",
                                activebackground=P.accent_hv, activeforeground="#ffffff",
                                relief="flat", bd=0, cursor="hand2",
                                font=(FAMILY, 10, "bold"), width=10, pady=5,
                                command=self._ok)
        self.ok_btn.pack(side="right", padx=(0, 10))

    def _center(self, root):
        self.update_idletasks()
        w, h = 360, 210
        x = root.winfo_rootx() + (root.winfo_width() - w) // 2
        y = root.winfo_rooty() + (root.winfo_height() - h) // 3
        self.geometry(f"+{max(x, 0)}+{max(y, 0)}")

    def _ok(self):
        name = self.name_var.get().strip()
        if not name:
            messagebox.showinfo("提示", "请输入群组名称", parent=self)
            return
        g = self.app.core.create_group(name)
        self.app._refresh_groups_list()
        self.app.select_gid(g["gid"])
        self.destroy()


# ----------------------------------------------------------------------------
# 改名投票对话框（其他电脑收到改名提案时弹出：同意 / 拒绝）
# ----------------------------------------------------------------------------
class RenameVoteDialog(tk.Toplevel):
    def __init__(self, app: App, info: dict):
        super().__init__(app.root)
        self.app = app
        self.pid = str(info["pid"])
        self.title("改名投票")
        self.configure(bg=P.panel)
        self.resizable(False, False)
        self.transient(app.root)
        try:
            self.attributes("-topmost", True)
        except Exception:
            pass
        try:
            self.grab_set()
        except Exception:
            pass
        need = app.core.vote_need
        body = tk.Frame(self, bg=P.panel, padx=22, pady=18)
        body.pack(fill="both", expand=True)
        tk.Label(body, text=f"「{info['from_name']}」申请改名", bg=P.panel, fg=P.text,
                 font=(FAMILY, 12, "bold")).pack(anchor="w")
        tk.Label(body, text=f"从「{info['old']}」改为「{info['new']}」", bg=P.panel, fg=P.text,
                 font=(FAMILY, 11)).pack(anchor="w", pady=(8, 2))
        tk.Label(body, text=f"按约定：首次改名自由，之后需超过 {need} 名用户同意。\n你同意这次改名吗？",
                 bg=P.panel, fg=P.sub, font=(FAMILY, 9), justify="left",
                 wraplength=320).pack(anchor="w", pady=(4, 14))
        btns = tk.Frame(body, bg=P.panel)
        btns.pack(fill="x")
        tk.Button(btns, text="拒绝", bg=P.panel, fg=P.sub, activebackground=P.hover,
                  relief="solid", bd=1, cursor="hand2", font=(FAMILY, 10),
                  width=10, pady=4, command=lambda: self._vote(False)).pack(side="right", padx=(8, 0))
        tk.Button(btns, text="同意", bg=P.accent, fg="#ffffff",
                  activebackground=P.accent_hv, activeforeground="#ffffff",
                  relief="flat", bd=0, cursor="hand2", font=(FAMILY, 10, "bold"),
                  width=10, pady=4, command=lambda: self._vote(True)).pack(side="right")
        self._center(app.root)
        self.protocol("WM_DELETE_WINDOW", lambda: None)   # 必须投票，不能直接关掉
        self.lift()
        self.focus_set()

    def _vote(self, approve: bool):
        self.app.core.cast_rename_vote(self.pid, approve)
        self.destroy()

    def _center(self, root):
        self.update_idletasks()
        w, h = 400, 210
        x = root.winfo_rootx() + root.winfo_width() - w - 40
        y = root.winfo_rooty() + root.winfo_height() - h - 60
        self.geometry(f"+{max(x, 0)}+{max(y, 0)}")


# ----------------------------------------------------------------------------
# 设置对话框（改名 / 更新检查 / 数据目录）
# ----------------------------------------------------------------------------
class SettingsDialog(tk.Toplevel):
    def __init__(self, app: App):
        super().__init__(app.root)
        self.app = app
        self._voting_new = None   # 正在投票的目标名字；None 表示当前没有投票
        app._settings_dlg = self
        self.title("设置")
        self.geometry("380x350")
        self.resizable(False, False)
        self.configure(bg=P.panel)
        self.grab_set()
        self.transient(app.root)
        self._center(app.root)

        body = tk.Frame(self, bg=P.panel, padx=22, pady=18)
        body.pack(fill="both", expand=True)
        tk.Label(body, text="设置", bg=P.panel, fg=P.text,
                 font=(FAMILY, 14, "bold")).pack(anchor="w", pady=(0, 12))

        tk.Label(body, text="我的名字（其他电脑看到的名字）", bg=P.panel, fg=P.sub,
                 font=(FAMILY, 9)).pack(anchor="w", pady=(4, 2))
        self.name_var = tk.StringVar(value=app.cfg["name"])
        ent = tk.Entry(body, textvariable=self.name_var, bg="#f7f7f7", fg=P.text,
                       relief="flat", bd=0, insertbackground=P.text,
                       font=(FAMILY, 11), highlightthickness=0)
        ent.pack(fill="x", ipady=6)
        ent.bind("<Return>", lambda e: self._save_name())
        if app.core.rename_needs_vote():
            tip = (f"提示：已改过一次名字，再次改名需超过 {app.core.vote_need} "
                   f"名用户投票同意后才能生效。")
        else:
            tip = "提示：第一次改名可以直接保存；之后的修改需要同学们投票。"
        self.tip_lbl = tk.Label(body, text=tip, bg=P.panel, fg=P.sub, font=(FAMILY, 8),
                                wraplength=330, justify="left")
        self.tip_lbl.pack(anchor="w", pady=(4, 0))

        row = tk.Frame(body, bg=P.panel)
        row.pack(fill="x", pady=(12, 4))
        self.save_btn = tk.Button(row, text="保存名字", bg=P.accent, fg="#ffffff",
                                  activebackground=P.accent_hv, activeforeground="#ffffff",
                                  relief="flat", bd=0, cursor="hand2",
                                  font=(FAMILY, 10, "bold"), padx=14, pady=5,
                                  command=self._save_name)
        self.save_btn.pack(side="left")
        self.saved_lbl = tk.Label(row, text="", bg=P.panel, fg=P.accent, font=(FAMILY, 9))
        self.saved_lbl.pack(side="left", padx=8)

        tk.Frame(body, height=1, bg=P.border).pack(fill="x", pady=12)
        for text, cmd in (("检查更新（发现新版本时提醒）", app._on_manual_update),
                          ("打开数据目录", app._open_data_dir)):
            tk.Button(body, text=text, bg=P.panel, fg=P.text, activebackground=P.hover,
                      relief="flat", bd=0, cursor="hand2", font=(FAMILY, 10),
                      anchor="w", command=cmd).pack(fill="x", pady=2)
        tk.Frame(body, height=1, bg=P.border).pack(fill="x", pady=10)
        tk.Label(body, text=f"{APP_NAME} v{APP_VERSION} · 局域网端口 "
                            f"UDP {app.cfg.get('udp_port', '?')} / TCP {app.core.my_tcp_port()}",
                 bg=P.panel, fg=P.sub, font=(FAMILY, 8)).pack(anchor="w")

    def _center(self, root):
        self.update_idletasks()
        w, h = 380, 350
        x = root.winfo_rootx() + (root.winfo_width() - w) // 2
        y = root.winfo_rooty() + (root.winfo_height() - h) // 3
        self.geometry(f"+{max(x, 0)}+{max(y, 0)}")

    def destroy(self):
        try:
            if getattr(self.app, "_settings_dlg", None) is self:
                self.app._settings_dlg = None
        except Exception:
            pass
        super().destroy()

    def _save_name(self):
        if self._voting_new is not None:
            return   # 投票进行中，按钮已禁用
        new = self.name_var.get().strip()
        if not new:
            messagebox.showinfo("提示", "名字不能为空", parent=self)
            return
        core = self.app.core
        if new == self.app.cfg["name"]:
            self.saved_lbl.configure(text="✓ 名字未改变")
            self.after(1200, lambda: self.saved_lbl.configure(text=""))
            return
        if not core.rename_needs_vote():
            # 第一次改名：直接生效
            core.rename(new)
            self._apply_ui(new)
            self.saved_lbl.configure(text="✓ 已保存（第一次改名）")
            self.after(1600, lambda: self.saved_lbl.configure(text=""))
            return
        # 第二次及以后：发起投票
        self._voting_new = new
        self.save_btn.configure(state="disabled")
        self.saved_lbl.configure(text=f"已发起投票：需超过 {core.vote_need} 名用户同意…")
        try:
            core.start_rename_vote(new)
        except Exception as e:
            self._voting_new = None
            self.save_btn.configure(state="normal")
            self.saved_lbl.configure(text="")
            messagebox.showerror("发起投票失败", str(e), parent=self)

    def _apply_ui(self, new: str):
        self.app.my_name_lbl.configure(text=new[:4])
        self.app._redraw_my_avatar()
        self.app._update_group_header()

    def on_vote_progress(self, pid, approve, reject):
        """投票进度（核心线程经 App 事件循环转发）。"""
        if self._voting_new is None:
            return
        self.saved_lbl.configure(
            text=f"投票中… 同意 {approve} 票（需超过 {self.app.core.vote_need} 票）")

    def on_vote_done(self, pid, ok, reason, new):
        """投票结束：通过则名字已更新，未通过提示原因。"""
        if self._voting_new is None:
            return
        self._voting_new = None
        self.save_btn.configure(state="normal")
        if ok:
            self._apply_ui(new)
            self.saved_lbl.configure(text="✓ 投票通过，名字已更新")
            self.after(2000, lambda: self.saved_lbl.configure(text=""))
        else:
            self.saved_lbl.configure(text="")
            messagebox.showwarning("改名未通过", f"{reason}\n\n可重新发起投票。", parent=self)


# ----------------------------------------------------------------------------
# 加入群组对话框（微信风格）
# ----------------------------------------------------------------------------
class JoinDialog(tk.Toplevel):
    def __init__(self, app: App):
        super().__init__(app.root)
        self.app = app
        self.title("加入群组")
        self.geometry("560x440")
        self.resizable(False, False)
        self.configure(bg=P.panel)
        self.grab_set()
        self.transient(app.root)
        self._center(app.root)

        frm = tk.Frame(self, bg=P.panel, padx=20, pady=16)
        frm.pack(fill="both", expand=True)
        tk.Label(frm, text="加入群组", bg=P.panel, fg=P.text,
                 font=(FAMILY, 14, "bold")).pack(anchor="w")
        tk.Label(frm, text="从局域网发现的群组（自动扫描，双击加入）", bg=P.panel, fg=P.sub,
                 font=(FAMILY, 9)).pack(anchor="w", pady=(4, 8))

        style = ttk.Style(self)
        try:
            style.configure("Join.Treeview", rowheight=30, font=(FAMILY, 10),
                            background=P.panel, fieldbackground=P.panel)
            style.configure("Join.Treeview.Heading", font=(FAMILY, 10, "bold"),
                            background=P.panel, foreground=P.sub)
            style.map("Join.Treeview", background=[("selected", P.selected)],
                      foreground=[("selected", P.text)])
        except Exception:
            pass
        cols = ("gname", "gid", "host", "members")
        self.tree = ttk.Treeview(frm, columns=cols, show="headings",
                                 style="Join.Treeview", height=8)
        for c, t, w in (("gname", "群名", 150), ("gid", "群号", 90),
                        ("host", "群主电脑", 130), ("members", "在线人数", 80)):
            self.tree.heading(c, text=t)
            self.tree.column(c, width=w, anchor="w")
        self.tree.pack(fill="both", expand=True, pady=(0, 8))
        self.tree.bind("<Double-1>", lambda e: self._join_selected())

        bar = tk.Frame(frm, bg=P.panel)
        bar.pack(fill="x")
        tk.Button(bar, text="加入所选群组", bg=P.accent, fg="#ffffff",
                  activebackground=P.accent_hv, activeforeground="#ffffff",
                  relief="flat", bd=0, cursor="hand2", font=(FAMILY, 10, "bold"),
                  padx=14, pady=4, command=self._join_selected).pack(side="left")
        tk.Button(bar, text="刷新", bg=P.panel, fg=P.sub, activebackground=P.hover,
                  relief="flat", bd=0, cursor="hand2", font=(FAMILY, 10),
                  padx=10, command=self._refresh).pack(side="left", padx=8)

        tk.Frame(frm, height=1, bg=P.border).pack(fill="x", pady=10)
        tk.Label(frm, text="或者手动输入群号加入：", bg=P.panel, fg=P.sub,
                 font=(FAMILY, 9)).pack(anchor="w")
        row = tk.Frame(frm, bg=P.panel)
        row.pack(fill="x", pady=4)
        self.gid_var = tk.StringVar()
        self.gname_var = tk.StringVar()
        tk.Label(row, text="群号", bg=P.panel, fg=P.text, font=(FAMILY, 10)).pack(side="left")
        tk.Entry(row, textvariable=self.gid_var, width=12, bg="#f7f7f7", relief="flat",
                 bd=0, insertbackground=P.text, font=(FAMILY, 10),
                 highlightthickness=0).pack(side="left", padx=6, ipady=4)
        tk.Label(row, text="群名（可留空）", bg=P.panel, fg=P.text, font=(FAMILY, 10)).pack(side="left")
        tk.Entry(row, textvariable=self.gname_var, width=16, bg="#f7f7f7", relief="flat",
                 bd=0, insertbackground=P.text, font=(FAMILY, 10),
                 highlightthickness=0).pack(side="left", padx=6, ipady=4)
        tk.Button(row, text="加入", bg=P.accent, fg="#ffffff",
                  activebackground=P.accent_hv, activeforeground="#ffffff",
                  relief="flat", bd=0, cursor="hand2", font=(FAMILY, 10, "bold"),
                  padx=12, pady=4, command=self._join_manual).pack(side="left", padx=4)

        self._refresh()
        self.after(1500, self._auto_refresh)

    def _center(self, root):
        self.update_idletasks()
        w, h = 560, 440
        x = root.winfo_rootx() + (root.winfo_width() - w) // 2
        y = root.winfo_rooty() + (root.winfo_height() - h) // 3
        self.geometry(f"+{max(x, 0)}+{max(y, 0)}")

    def _auto_refresh(self):
        if self.winfo_exists():
            self._refresh()
            self.after(3000, self._auto_refresh)

    def _refresh(self):
        self.tree.delete(*self.tree.get_children())
        dis = self.app.core.discovered
        for gid, g in sorted(dis.items(), key=lambda kv: kv[1]["gname"]):
            online = sum(1 for p in self.app.core.peers.values()
                         if now_ts() - (p.get("ts") or 0) <= PRESENCE_TTL
                         and any(x.get("gid") == gid for x in p.get("groups", [])))
            self.tree.insert("", tk.END, iid=gid,
                             values=(g["gname"], gid, g["host_name"], str(online)))

    def _join_selected(self):
        sel = self.tree.selection()
        if not sel:
            return
        gid = sel[0]
        gname = self.tree.item(gid, "values")[0]
        self.app.core.join_group(gid, gname)
        self.app._refresh_groups_list()
        self.app.select_gid(gid)
        self.destroy()

    def _join_manual(self):
        gid = self.gid_var.get().strip()
        gname = self.gname_var.get().strip()
        if not gid:
            messagebox.showinfo("提示", "请输入群号（向创建者索取）", parent=self)
            return
        self.app.core.join_group(gid, gname)
        self.app._refresh_groups_list()
        self.app.select_gid(self.app.core.groups[gid.upper()]["gid"])
        self.destroy()


# ----------------------------------------------------------------------------
def main():
    root = tk.Tk()
    try:
        App(root)
    except Exception:
        traceback.print_exc()
        messagebox.showerror("启动失败", "程序启动失败，请查看控制台错误信息。")
        return
    root.mainloop()


if __name__ == "__main__":
    main()
