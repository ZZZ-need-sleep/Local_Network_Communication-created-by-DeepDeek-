# -*- coding: utf-8 -*-
"""无头 Python 对端 —— 供 C++ 版做「协议完全兼容、可混用」的跨语言互通测试使用。

用法示例：
  python tools/py_peer.py --data-dir _itdata/py --name 设备P --uid PPPPPPPPPPPP \
      --udp 46279 --tcp 46281 --group G1:班群 --log "Python 对端上线" \
      --out _itdata/py_state.json --seconds 25

它会：设置 NEIWANG_* 环境变量 → 导入 neiwang → 建立 PeerCore → 可选建内网/加群/预置文件
→ 每 0.5 秒把自身状态与收到的事件写成 JSON 到 --out，供 C++ 测试轮询断言。

状态文件结构：
{
  "uid": "...", "name": "...", "tcp_port": 46281, "in_net": true, "net_nid": "...",
  "net_members": ["..."], "peers": {"uid": {...}}, "groups": [{"gid":..,"gname":..}],
  "log_lines": ["..."], "events": [[kind, ...], ...], "act_events": [...]
}
"""
import argparse
import hashlib
import json
import os
import queue
import shutil
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--name", default="Python对端")
    ap.add_argument("--uid", default="PYTEST000001")
    ap.add_argument("--udp", type=int, default=46279)
    ap.add_argument("--tcp", type=int, default=46281)
    ap.add_argument("--group", default="", help="GID:群名，可多次用逗号分隔")
    ap.add_argument("--net", default="", help="启动时自建内网的名字")
    ap.add_argument("--auto-join", action="store_true", help="扫描附近内网并申请加入")
    ap.add_argument("--preload", default="", help="预置一个文件到 cache/（用于 get 互通测试）")
    ap.add_argument("--log", default="", help="启动后广播一条公共日志")
    ap.add_argument("--chat", default="", help="GID:文本，入网加群后向该群发一条群聊")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=25.0)
    ap.add_argument("--act-hello", action="store_true", help="启动后广播一次 act_hello")
    args = ap.parse_args()

    data_dir = Path(args.data_dir)
    data_dir.mkdir(parents=True, exist_ok=True)

    os.environ["NEIWANG_DATA_DIR"] = str(data_dir.resolve())
    os.environ["NEIWANG_UDP_PORT"] = str(args.udp)
    os.environ["NEIWANG_TCP_PORT"] = str(args.tcp)
    os.environ.setdefault("NEIWANG_PRES_INTERVAL", "3")
    os.environ.setdefault("NEIWANG_SCAN_WINDOW", "2")
    os.environ.setdefault("NEIWANG_NET_ANNOUNCE", "3")
    os.environ.setdefault("NEIWANG_NET_REQ_REBROADCAST", "2")

    # 仓库结构：<root>/Python/neiwang.py、<root>/C++/...、<root>/tools/py_peer.py
    sys.path.insert(0, str(ROOT / "Python"))
    import neiwang  # noqa: E402

    neiwang.DATA_DIR = data_dir
    cfg = neiwang.load_config()
    cfg["uid"] = args.uid
    cfg["name"] = args.name
    cfg["tcp_port"] = args.tcp
    cfg["udp_port"] = args.udp
    cfg["update_url"] = "http://127.0.0.1:1/none"
    cfg["update_page"] = ""
    cfg["groups"] = []
    neiwang.save_config(cfg)

    # 预置文件到 cache/（供 C++ 端 cmd=get 互通测试）
    if args.preload and os.path.isfile(args.preload):
        content = Path(args.preload).read_bytes()
        fid = hashlib.sha256(content).hexdigest()
        cache = data_dir / "cache"
        cache.mkdir(parents=True, exist_ok=True)
        (cache / fid).write_bytes(content)
        (data_dir / "preload.json").write_text(
            json.dumps({"file_id": fid, "name": Path(args.preload).name,
                        "size": len(content)}, ensure_ascii=False), encoding="utf-8")

    events: "queue.Queue" = queue.Queue()
    core = neiwang.PeerCore(cfg, events)

    # 必须先 start()（建立 UDP/TCP socket），再建内网/加群，否则 _broadcast 时 socket 还未创建
    core.start()

    if args.net:
        try:
            core.net_create(args.net)
        except Exception as exc:  # noqa: BLE001
            print("net_create failed:", exc, file=sys.stderr)

    if args.auto_join:
        # 等对端（C++ 版）先建好内网，再扫描并申请加入；加入成功后才加群（加群要求先入网）
        deadline = time.time() + 20.0
        applied = False
        while time.time() < deadline and not core.in_net():
            try:
                core.net_scan(1.5)
                found = core.net_nearby_list()
                if found and not applied:
                    core.net_apply_join(found[0]["nid"])
                    applied = True
            except Exception as exc:  # noqa: BLE001
                print("auto_join retry:", exc, file=sys.stderr)
            if core.in_net():
                break
            time.sleep(0.4)

    for spec in [s for s in args.group.split(",") if s.strip()]:
        gid, _, gname = spec.partition(":")
        try:
            core.join_group(gid.strip(), gname.strip() or gid.strip())
        except Exception as exc:  # noqa: BLE001
            print("join_group failed:", exc, file=sys.stderr)

    if args.log:
        time.sleep(1.5)
        try:
            core.broadcast_log(args.log)
        except Exception:  # noqa: BLE001
            pass

    if args.act_hello:
        time.sleep(0.5)
        try:
            core.record_activity("log", f"{args.name}: Python 侧活动记录")
            core._act_hello_broadcast()
        except Exception:  # noqa: BLE001
            pass

    chat_gid, _, chat_text = args.chat.partition(":")
    chat_gid = chat_gid.strip()
    chat_text = chat_text.strip() or "hello"

    collected = []
    out_path = Path(args.out)
    deadline = time.time() + args.seconds
    next_chat = time.time() + 2.0
    while time.time() < deadline:
        # 对端（C++ 版）可能晚一点才加入群，所以群聊周期性重发，保证可被观测到
        if chat_gid and time.time() >= next_chat:
            try:
                core.send_chat(chat_gid, chat_text)
            except Exception as exc:  # noqa: BLE001
                print("send_chat failed:", exc, file=sys.stderr)
            next_chat = time.time() + 6.0
        try:
            while True:
                ev = events.get_nowait()
                collected.append(list(ev))
        except queue.Empty:
            pass
        state = {
            "uid": args.uid,
            "name": args.name,
            "tcp_port": core.my_tcp_port(),
            "in_net": core.in_net(),
            "net_nid": (core.net or {}).get("nid", "") if core.in_net() else "",
            "net_members": sorted(core.net_members.keys()),
            "peers": {u: {"name": p.get("name"), "ip": p.get("ip"),
                          "tcp_port": p.get("tcp_port"),
                          "groups": p.get("groups", [])}
                      for u, p in list(core.peers.items())},
            "groups": core.my_groups(),
            "log_lines": [ev[1] for ev in collected if ev and ev[0] == "log"][-200:],
            "activity": core.activity_view() if hasattr(core, "activity_view") else {},
            "proxy_count": len(core.proxy_view()) if hasattr(core, "proxy_view") else 0,
            "events": collected[-300:],
        }
        tmp = out_path.with_suffix(out_path.suffix + ".tmp")
        tmp.write_text(json.dumps(state, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, out_path)
        time.sleep(0.5)

    try:
        core.stop()
    except Exception:  # noqa: BLE001
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
