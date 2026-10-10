# -*- coding: utf-8 -*-
"""
内部网 局域网群组 —— 更新程序1（design_v2 第 4 节）
===============================================
这是**单独的一个程序**：可以脱离主程序单独运行（Windows 双击「更新.bat」）。

更新四步（全部自动）：
    1) 把 data/（用户所有会话内容：群组、聊天、文件索引、转存记录）备份到 customer_god/；
    2) 把新版本取到 update_clone/：
         - 装了 Git 且配置了 update_repo  -> git clone --depth 1
         - 否则配置了 update_package      -> 下载 ZIP 更新包并解压（不需要 Git，适合内网部署）
    3) 把 customer_god/ 里的会话内容回填到新版本的 data/ 目录；
    4) **就地更新全部**：从 update_clone/ 把除 data/、customer_god/、update_clone/ 与
       更新程序1 自己以外的全部文件覆盖到程序目录，并删除属于「程序文件」的旧文件。

第五步（更新程序1 更新自己）由更新程序2 完成：
    更新程序1 就地更新完成后，带 --wait-pid <pid> 调起 neiwang_update2.py，
    由它把 update_clone/neiwang_update1.* 覆盖到还在运行的更新程序1 上。

用法：
    python neiwang_update1.py            # 手动模式：一定给出明确反馈
    python neiwang_update1.py --auto     # 静默：只有发现新版本才弹窗
    python neiwang_update1.py --update   # 不询问，直接开始更新
    python neiwang_update1.py --check    # 无界面：只打印一行 UPDATE_AVAILABLE/UP_TO_DATE/OFFLINE

零第三方依赖，Python 3.8+ 即可。
"""

import sys
import traceback
from pathlib import Path

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
    cmd_check, console_mode, usage, TK_AVAILABLE,
)
if C.TK_AVAILABLE:
    from neiwang_update_common import UpdaterApp as _CommonUpdaterApp

    class UpdaterApp(_CommonUpdaterApp):
        """更新程序1：就地更新全部。"""
        ROLE = 1
        UPDATER_FILE = UPDATER1_FILE
else:
    UpdaterApp = None


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    flags = {a.strip().lower() for a in argv}

    # --root <path>：与 C++ 版参数保持一致（等价于 NEIWANG_UPDATE_ROOT）
    try:
        apply_root_arg(argv)
    except NameError:
        pass

    if flags & {"-h", "--help", "/?"}:
        print(usage(UPDATER1_FILE, role=1))
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
        return console_mode(cfg, auto=(mode == "auto"), role=1)

    try:
        app = UpdaterApp(mode, cfg)
    except Exception:
        traceback.print_exc()
        if mode == "auto":
            return 0
        return console_mode(cfg, auto=False, role=1)

    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
