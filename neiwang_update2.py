# -*- coding: utf-8 -*-
"""
内部网 局域网群组 —— 更新程序2（design_v2 第 4 节）
===============================================
更新程序2 只做一件事：**更新更新程序1 自己**。

    python neiwang_update2.py --patch-updater1 [--wait-pid <pid>]
        不检查、不下载，直接把 update_clone/ 里的更新程序1（neiwang_update1.*）
        覆盖到程序目录。--wait-pid 用于等待正在运行的更新程序1 退出（超时 30 秒）；
        仍失败则把新文件写成 neiwang_update1.new.<后缀> 并提示重启。

普通模式（无参数 / --auto / --update / --check）与更新程序1 完全一致：
    python neiwang_update2.py --check    # 只打印一行 UPDATE_AVAILABLE/UP_TO_DATE/OFFLINE
真正的更新（备份 / 取包 / 回填 / 就地覆盖）由更新程序1 完成，本程序会替你启动它。

零第三方依赖，Python 3.8+ 即可。
"""

import sys
import traceback

import neiwang_update_common as C

# 公共逻辑再导出（便于单独运行 / 测试直接使用）
from neiwang_update_common import (        # noqa: F401
    APP_NAME, MAIN_PROGRAM_NAME, UPDATE_MANIFEST_NAME, UPDATER1_FILE, UPDATER2_FILE,
    UPDATER_PID_FILE, APPLIED_MARKER, CUSTOMER_DATA_DIR, UPDATE_CLONE_DIR,
    PROGRAM_DIR, get_data_dir, update_root, parse_update_manifest, version_key,
    is_newer_version, local_manifest_path, local_version, load_config, have_git,
    fetch_manifest, check_for_update, backup_customer_data, restore_customer_data,
    download_file, safe_extract, single_root, run_git_clone, fetch_new_version,
    perform_update, inplace_update, is_program_file, write_applied_marker,
    find_updater, launch_updater2, launch_updater1, pid_alive, wait_for_pid,
    write_pid_file, read_pid_file, append_clone_log, open_folder, launch_new_version,
    cmd_check, console_mode, usage, patch_updater1, TK_AVAILABLE,
)
if C.TK_AVAILABLE:
    from neiwang_update_common import UpdaterApp as _CommonUpdaterApp

    class UpdaterApp(_CommonUpdaterApp):
        """更新程序2：普通模式下把真正的更新交给更新程序1。"""
        ROLE = 2
        UPDATER_FILE = UPDATER2_FILE
else:
    UpdaterApp = None


def parse_wait_pid(argv: list):
    """解析 --wait-pid <pid> / --wait-pid=<pid>。"""
    for i, a in enumerate(argv):
        s = str(a).strip().lower()
        if s in ("--wait-pid", "-w") and i + 1 < len(argv):
            try:
                return int(argv[i + 1])
            except Exception:
                return None
        if s.startswith("--wait-pid="):
            try:
                return int(a.split("=", 1)[1])
            except Exception:
                return None
    return None


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    flags = {a.strip().lower() for a in argv}

    # --root <path>：与 C++ 版参数保持一致（等价于 NEIWANG_UPDATE_ROOT）
    try:
        C.apply_root_arg(argv)
    except Exception:
        pass

    if flags & {"-h", "--help", "/?"}:
        print(usage(UPDATER2_FILE, role=2))
        return 0

    # 更新程序2 的唯一职责：把 update_clone/ 里的更新程序1 覆盖到程序目录
    if "--patch-updater1" in flags:
        res = patch_updater1(log=print, wait_pid=parse_wait_pid(argv))
        if not res.get("ok"):
            print("PATCH_UPDATER1_FAILED " + str(res.get("reason", "")))
            return 1
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

    if not TK_AVAILABLE or UpdaterApp is None:
        return console_mode(cfg, auto=(mode == "auto"), role=2)

    try:
        app = UpdaterApp(mode, cfg)
    except Exception:
        traceback.print_exc()
        if mode == "auto":
            return 0
        return console_mode(cfg, auto=False, role=2)

    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
