# -*- coding: utf-8 -*-
"""更新隔离性测试：验证「C++ 版只更新 C++ 版、Python 版只更新 Python 版」。

背景：仓库根下有 Python/ 与 C++/ 两个平级版本，各自有独立的 update.wenyi 与独立的
更新目录（customer_god/、update_clone/）。本脚本构造一个「假仓库」并打成 ZIP，
分别交给两版的更新程序，检查：

  1. Python 版更新程序：能把「仓库根结构」的 ZIP（根下有 C++/ 与 Python/）与
     「版本目录结构」的 ZIP 都正确识别，更新后 update_clone/neiwang.py 存在，
     且 **不会**把 C++/ 目录带进来；
  2. C++ 版更新程序：同样支持两种 ZIP 结构，就地更新后 C++ 安装目录里的
     build/neiwang.exe 被换成新版，且 **不会**碰同级的 Python/ 目录。

用法：python tools/test_update_isolation.py
"""
import http.server
import json
import os
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY_DIR = ROOT / "Python"
CPP_DIR = ROOT / "C++"
CPP_UPDATER1 = CPP_DIR / "build" / "neiwang_update1.exe"

NEW_PY_VER = "9.9.1"
NEW_CPP_VER = "9.9.1"

passed = 0
failed = 0
fails = []


def check(cond, what, extra=""):
    global passed, failed
    if cond:
        passed += 1
        print("PASS", what)
    else:
        failed += 1
        fails.append(what)
        print("FAIL", what, extra)


def build_fake_repo(base: Path) -> Path:
    """构造仓库根：C++/ 与 Python/ 两个版本目录。"""
    c = base / "C++"
    (c / "build").mkdir(parents=True, exist_ok=True)
    (c / "src").mkdir(parents=True, exist_ok=True)
    (c / "update.wenyi").write_text(f"{NEW_CPP_VER}\n- C++ 版新版本（测试）\n", encoding="utf-8")
    (c / "build" / "neiwang.exe").write_bytes(b"FAKE-CPP-EXE-" + NEW_CPP_VER.encode())
    (c / "build" / "neiwang_update1.exe").write_bytes(b"FAKE-CPP-UPD1-" + NEW_CPP_VER.encode())
    (c / "src" / "main.cpp").write_text("// cpp new\n", encoding="utf-8")
    (c / "marker_cpp.txt").write_text("cpp-only\n", encoding="utf-8")

    p = base / "Python"
    p.mkdir(parents=True, exist_ok=True)
    (p / "neiwang.py").write_text("# fake new neiwang\n", encoding="utf-8")
    (p / "neiwang_update1.py").write_text("# fake new updater1\n", encoding="utf-8")
    (p / "update.wenyi").write_text(f"{NEW_PY_VER}\n- Python 版新版本（测试）\n", encoding="utf-8")
    (p / "marker_py.txt").write_text("py-only\n", encoding="utf-8")
    return base


def zip_tree(src: Path, out: Path) -> None:
    """把 src 目录下的内容打进 zip（条目以 src 为根）。"""
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        for item in sorted(Path(src).rglob("*")):
            if item.is_file():
                zf.write(item, item.relative_to(src).as_posix())


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):  # 静音
        pass


def serve(directory: Path):
    handler = lambda *a, **kw: Handler(*a, directory=str(directory), **kw)  # noqa: E731
    httpd = socketserver.TCPServer(("127.0.0.1", 0), handler)
    port = httpd.socket.getsockname()[1]
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd, port


def main() -> int:
    # 注意：工作目录必须放在仓库内（沙箱只允许写工作区），不要用系统临时目录
    tmp = ROOT / "_updtest"
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True, exist_ok=True)
    try:
        repo = build_fake_repo(tmp / "repo")
        # 两种 ZIP：仓库根结构 / 版本目录结构
        zip_repo = tmp / "repo_root.zip"
        zip_tree(repo, zip_repo)
        zip_cpp_only = tmp / "cpp_only.zip"
        zip_tree(repo / "C++", zip_cpp_only)
        zip_py_only = tmp / "py_only.zip"
        zip_tree(repo / "Python", zip_py_only)

        httpd, port = serve(tmp)
        base_url = f"http://127.0.0.1:{port}"

        # ---------------------------------------------------- 1) Python 版更新
        print("=== 1) Python 版只更新 Python 版 ===")
        sys.path.insert(0, str(PY_DIR))
        os.environ["NEIWANG_UPDATE_ROOT"] = str(tmp / "pywork")
        (tmp / "pywork").mkdir(exist_ok=True)
        import neiwang_update_common as C  # noqa: E402
        C.PROGRAM_DIR = PY_DIR

        for label, pkg in (("仓库根结构的 ZIP", zip_repo.name),
                           ("版本目录结构的 ZIP", zip_py_only.name)):
            work = tmp / ("pywork_" + ("root" if "repo" in pkg else "ver"))
            work.mkdir(exist_ok=True)
            os.environ["NEIWANG_UPDATE_ROOT"] = str(work)
            cfg = {"update_repo": "", "update_package": f"{base_url}/{pkg}"}
            res = C.fetch_new_version(cfg, work / "update_clone")
            clone = work / "update_clone"
            check(res.get("ok"), f"Python 版取回新版本成功（{label}）", str(res))
            check((clone / "neiwang.py").is_file(), f"Python 版新版本含 neiwang.py（{label}）")
            check((clone / "marker_py.txt").is_file(), f"Python 版新版本内容完整（{label}）")
            check(not (clone / "C++").exists(),
                  f"Python 版新版本里没有被塞进 C++/ 目录（{label}）")
            check((clone / "update.wenyi").read_text(encoding="utf-8").splitlines()[0] == NEW_PY_VER,
                  f"Python 版新版本清单版本号正确（{label}）")

        # ---------------------------------------------------- 2) C++ 版更新
        print("=== 2) C++ 版只更新 C++ 版 ===")
        if not CPP_UPDATER1.is_file():
            check(False, "C++ 更新程序存在（先运行 C++\\build.bat app）", str(CPP_UPDATER1))
        else:
            for label, pkg in (("仓库根结构的 ZIP", zip_repo.name),
                               ("版本目录结构的 ZIP", zip_cpp_only.name)):
                inst = tmp / ("cppinst_" + ("root" if "repo" in pkg else "ver"))
                # 模拟一个已安装的 C++ 版：C++/ 目录内容 + 一个「不该被动到」的同级 Python/ 目录
                (inst / "build").mkdir(parents=True, exist_ok=True)
                (inst / "src").mkdir(parents=True, exist_ok=True)
                (inst / "data").mkdir(parents=True, exist_ok=True)
                (inst / "Python").mkdir(parents=True, exist_ok=True)
                (inst / "build" / "neiwang.exe").write_bytes(b"OLD-CPP-EXE")
                (inst / "build" / "neiwang_update1.exe").write_bytes(b"OLD-UPD1")
                (inst / "build" / "neiwang_update2.exe").write_bytes(b"OLD-UPD2")
                (inst / "src" / "main.cpp").write_text("// old\n", encoding="utf-8")
                (inst / "update.wenyi").write_text("1.0.0\n- 旧版\n", encoding="utf-8")
                (inst / "Python" / "marker_keep.txt").write_text("do-not-touch\n", encoding="utf-8")
                (inst / "data" / "config.json").write_text(json.dumps({
                    "update_url": f"{base_url}/{pkg}",
                    "update_page": "",
                    "update_repo": "",
                    "update_package": f"{base_url}/{pkg}",
                }, ensure_ascii=False), encoding="utf-8")

                p = subprocess.run([str(CPP_UPDATER1), "--update", "--root", str(inst)],
                                   capture_output=True, text=True, timeout=180)
                logf = inst / "update_clone.log"
                if logf.is_file():
                    print("    [C++ 更新程序日志 update_clone.log]")
                    for ln in logf.read_text(encoding="utf-8", errors="replace").splitlines()[-12:]:
                        print("      " + ln)
                print("    [C++ 更新程序] exit=", p.returncode,
                      ("stderr=" + (p.stderr or "").strip()[:200]) if p.stderr else "")
                # 更新程序1 会再去调更新程序2（这里允许它失败），因此只看文件结果
                exe = (inst / "build" / "neiwang.exe")
                check(exe.is_file() and exe.read_bytes() == b"FAKE-CPP-EXE-" + NEW_CPP_VER.encode(),
                      f"C++ 安装目录的 build/neiwang.exe 已换成新版（{label}）",
                      f"exit={p.returncode} err={p.stderr[-200:]}")
                ver_line = (inst / "update.wenyi").read_text(encoding="utf-8").splitlines()[0]
                check(ver_line == NEW_CPP_VER,
                      f"C++ 安装目录的 update.wenyi 已更新（{label}）", ver_line)
                check((inst / "marker_cpp.txt").is_file(),
                      f"C++ 新版本文件已就地覆盖（{label}）")
                keep = inst / "Python" / "marker_keep.txt"
                check(keep.is_file() and keep.read_text(encoding="utf-8") == "do-not-touch\n",
                      f"C++ 更新程序没有碰同级的 Python/ 目录（{label}）")
                check((inst / "data" / "config.json").is_file(),
                      f"C++ 更新程序保留了本机 data/ 目录（{label}）")

        httpd.shutdown()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("=" * 60)
    print(f"结果：{passed}/{passed + failed} 通过")
    if fails:
        print("失败项：" + "；".join(fails))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
