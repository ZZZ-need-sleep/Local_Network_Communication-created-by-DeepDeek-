# 04 更新程序与测试 —— C++/Win32 移植精确规格

> 本文是**规格文档，不含 C++ 代码**。所有事实均逐行来自工作区中的实际源码：
> `neiwang_update.py`（748 行）、`neiwang.py`（4760 行，仅更新相关部分）、
> `test_protocol.py`(319 行)、`test_net.py`(308 行)、`test_ui_net.py`(183 行)、
> `test_gui_smoke.py`(444 行)、`update.wenyi`、`启动.bat`、`更新.bat`。
> 凡源码中不存在的信息一律未写入；不确定处标注「源码未规定」。

---

## 0. 名词、常量与文件清单

### 0.1 `neiwang_update.py` 顶层常量（第 47–72 行）

| 常量名 | 值 | 用途 |
| --- | --- | --- |
| `APP_NAME` | `"内部网"` | 界面标题、usage 文案 |
| `UPDATER_FILE` | `"neiwang_update.py"` | usage 文案里显示的程序名 |
| `MAIN_PROGRAM_NAME` | `"neiwang.py"` | **校验取回的新版本是否为完整程序** |
| `UPDATE_MANIFEST_NAME` | `"update.wenyi"` | 清单文件名 |
| `CUSTOMER_DATA_DIR` | `"customer_god"` | 会话内容备份目录名 |
| `UPDATE_CLONE_DIR` | `"update_clone"` | 新版本目录名 |
| `ZIP_NAME` | `"update_package.zip"` | 下载下来的 ZIP 名 |
| `UNZIP_DIR` | `"_update_unzip"` | 解压临时目录名 |
| `PROGRAM_DIR` | `Path(__file__).resolve().parent` | 程序目录（**不是**当前工作目录） |
| `GITHUB_OWNER` | `"ZZZ-need-sleep"` | |
| `GITHUB_REPO` | `"Local_Network_Communication-created-by-DeepDeek-"` | |
| `GITHUB_BRANCH` | `"main"` | |
| `DEFAULT_UPDATE_URL` | `https://raw.githubusercontent.com/{owner}/{repo}/main/update.wenyi` | 默认清单地址 |
| `DEFAULT_UPDATE_REPO` | `https://github.com/{owner}/{repo}.git` | 默认 Git 仓库 |
| `DESKTOP_UPDATE_PAGE` | `https://github.com/{owner}/{repo}` | 默认发布页 |
| `CHECK_TIMEOUT` | `8`（秒） | 清单请求超时 |
| `DOWNLOAD_TIMEOUT` | `300`（秒） | ZIP 下载超时 |
| `GIT_TIMEOUT` | `900`（秒） | git clone 超时 |

展开后的三个默认地址（逐字）：

```
update_url  = https://raw.githubusercontent.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-/main/update.wenyi
update_repo = https://github.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-.git
update_page = https://github.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-
```

### 0.2 主程序 `neiwang.py` 侧对应常量（第 64–87 行）

`APP_NAME="内部网"`、`APP_VERSION="1.1.0"`（**兜底值**）、`UPDATE_MANIFEST_NAME="update.wenyi"`、
`MAIN_PROGRAM_NAME="neiwang.py"`、`UPDATER_PROGRAM_NAME="neiwang_update.py"`、
`CUSTOMER_DATA_DIR="customer_god"`、`UPDATE_CLONE_DIR="update_clone"`。
GitHub 三常量与三个默认地址与更新程序**逐字一致**。

### 0.3 环境变量（测试钩子，必须逐字复刻）

| 变量 | 读取处 | 作用 |
| --- | --- | --- |
| `NEIWANG_DATA_DIR` | `neiwang_update.get_data_dir()`；`neiwang.get_data_dir()` | 覆盖数据目录（默认 `<程序目录>/data`）。取到值就 `mkdir(parents=True, exist_ok=True)` |
| `NEIWANG_UPDATE_ROOT` | `neiwang_update.update_root()` | 覆盖更新工作目录（默认 `<程序目录>`）。**不创建目录** |
| `NEIWANG_UDP_PORT` / `NEIWANG_TCP_PORT` | `neiwang` 端口默认值 | 测试加速/避开占用 |
| `NEIWANG_PRES_INTERVAL` / `NEIWANG_PRES_TTL` / `NEIWANG_OFFER_WAIT` | `neiwang` | 心跳间隔 / 在线判定 TTL / 公共询问等待 |
| `NEIWANG_VOTE_NEED` / `NEIWANG_VOTE_WAIT` | `neiwang` | 改名投票门槛 / 等待 |
| `NEIWANG_SCAN_WINDOW` / `NEIWANG_NET_ANNOUNCE` / `NEIWANG_NET_REQ_REBROADCAST` | `neiwang` | 内网扫描窗口 / 公告周期 / 申请重播周期 |
| `NEIWANG_NET_VOTE_WINDOW` / `NEIWANG_NET_VOTE_CHANGE` / `NEIWANG_NET_NEARBY_TTL` | `neiwang` | 入网投票有效期 / 可改票窗口 / 附近内网 TTL |
| `PYTHONDONTWRITEBYTECODE` | 解释器 | 测试里设为 `1`，避免生成 `__pycache__` |

### 0.4 更新程序产生的文件（`<update_root>` 下）

| 路径 | 产生时机 | 说明 |
| --- | --- | --- |
| `customer_god/` | 第 1 步 | 会话内容备份目录（`copytree` 源 = data 目录） |
| `update_clone/` | 第 2 步 | 新版本目录 |
| `update_clone.log` | 仅 Git 路径 | `git clone` 的 stdout+stderr 合并写入；**每次以 `"w"` 截断** |
| `update_package.zip` | 仅 ZIP 路径 | 下载的更新包 |
| `_update_unzip/` | 仅 ZIP 路径 | 解压临时目录；`finally` 中 `rmtree(ignore_errors=True)` 删除 |

`.gitignore`（第 20–24 行附近）把 `customer_god/`、`update_clone/`、`update_clone.log`、
`update_package.zip`、`_update_unzip/` 全部忽略，测试临时目录 `_testdata*/` 也忽略。

---

## 1. 命令行参数规格（逐字）

### 1.1 解析算法（`main`，第 712–744 行）

```
argv  = 进程参数去掉 argv[0]（若调用方传入则用传入值）
flags = { 每个参数 .strip().lower() }        # 集合，顺序无关

1) if flags ∩ {"-h","--help","/?"} != ∅ :  打印 usage() 到 stdout；return 0   ← 最先判定
2) cfg = load_config()
3) if flags ∩ {"-c","--check"} != ∅ :      return cmd_check(cfg)             ← 第二判定
4) mode = "auto"    if flags ∩ {"-a","--auto"}   != ∅
       = "update"  elif flags ∩ {"-u","--update"} != ∅
       = "manual"  否则
5) if tkinter 不可用:  return console_mode(cfg, auto=(mode=="auto"))
6) try: app = UpdaterApp(mode, cfg)
   except: 打印 traceback 到 stderr；auto 模式 return 0；否则 return console_mode(cfg, auto=False)
7) app.mainloop(); return 0
```

要点：
- **参数是全量集合匹配，不是前缀匹配**；大小写不敏感；多余未知参数被忽略，不报错。
- `--auto` 优先于 `--update`（`--auto --update` → auto）。
- `--check` 优先于一切模式参数；`--help` 优先于 `--check`。
- 已知等价写法：`-h`/`--help`/`/?`；`-c`/`--check`；`-a`/`--auto`；`-u`/`--update`。

### 1.2 `usage()` 输出（第 703–709 行，逐字）

```
内部网 更新程序（独立程序，可单独运行）
用法：
  python neiwang_update.py           检查并更新（有界面）
  python neiwang_update.py --auto    静默检查：无网络/版本相同不打扰，有新版本才弹窗
  python neiwang_update.py --update  直接开始更新
  python neiwang_update.py --check   无界面检查，打印一行结果
```

（`UPDATER_FILE`/`APP_NAME` 为变量插值；上面是其实际展开文本。）

### 1.3 四种运行形态总表

| 调用 | 模式 | 界面 | 退出码 | stdout |
| --- | --- | --- | --- | --- |
| 无参数 | `manual` | 有窗口（560×460，居中由系统决定），**始终给出明确反馈** | `mainloop()` 结束（用户关窗）后 `0` | 无（GUI 模式不打印） |
| `--auto` / `-a` | `auto` | **先 withdraw 隐藏**；仅「发现新版本」时 `deiconify()+lift()` | 关窗后 `0`；`current`/`offline` 走 `destroy()` 直接退出 | 无 |
| `--update` / `-u` | `update` | 有窗口，**启动即 `_begin_update()`，不检查、不询问** | 关窗后 `0` | 无 |
| `--check` / `-c` | 无界面 | 无 | **恒为 `0`** | 恰好一行（见 1.4） |
| `--help` / `-h` / `/?` | — | 无 | `0` | `usage()` 文本 |

无 tkinter 环境下的**控制台回退**（`console_mode`，第 671–700 行）：
- `current`：非 auto 时打印 `已经是最新版本（v{local}）。`；auto 时静默；两种都 `return 0`。
- `offline`：非 auto 时打印 `无法连接更新服务器：{reason}`；auto 时静默；两种都 `return 0`。
- `update`：
  - 打印 `发现新版本 —— v{remote}（当前 v{local}）`；
  - 若 `notes` 非空：打印 `更新内容：` 换行后打印 `notes`；
  - auto 模式：再打印 `可以用带界面的方式运行本程序来更新：python neiwang_update.py`，`return 0`；
  - manual 模式：`input("是否现在更新？[y/N] ")`，`.strip().lower()` 属于 `{"y","yes"}` 才更新，
    否则打印 `已取消。` 并 `return 0`；输入异常（EOF 等）按 `"n"` 处理。
  - 执行更新后：成功打印 `更新完成：{path}` 返回 `0`；失败打印 `更新失败：{reason}` 返回 `1`
    （**控制台回退是唯一可能返回非 0 的路径**）。

### 1.4 `cmd_check` 输出格式（第 658–668 行，逐字）

```
status == "update"  ->  UPDATE_AVAILABLE {remote}
status == "current" ->  UP_TO_DATE {local}
否则（offline）      ->  OFFLINE
```

- 每种情况**恰好一行**，行尾换行由 `print` 追加；恒 `return 0`。
- `remote` 是远程清单里的版本字符串；`local` 是本地 `update.wenyi` 首行的版本字符串。
- 注意 `UP_TO_DATE` 打印的是**本地**版本号（而 `UPDATE_AVAILABLE` 打印**远程**版本号）。

---

## 2. 更新清单 `update.wenyi` 的解析规则

### 2.1 `parse_update_manifest(data) -> {"version": str, "notes": str}`（第 92–117 行）

```
1. data = (data or "").strip()
2. if data == "": 返回 {"version": "", "notes": ""}
3. 尝试 JSON：
     把 data 作为 JSON 解析；若结果是 object 且含键 "version"：
         ver   = str(obj.get("version","")).strip()          # 数字也会被转成字符串再 strip
         notes = obj.get("notes") or obj.get("content") or obj.get("changelog") or ""
                 # 按此顺序取第一个「真值」（Python falsy 规则：""、[]、0、null、{} 都跳过）
         若 notes 是数组 -> 用 "\n" 连接每个元素的 str()
         否则            -> str(notes).strip()
         返回 {"version": ver, "notes": notes}
     若 JSON 解析抛异常 或 不是 object 或 不含 "version"：静默落入纯文本分支
4. 纯文本分支：
     lines = data.splitlines()          # 已整体 strip，首行不会因空行而错位
     ver   = lines[0].strip()
     notes = "\n".join(lines[1:]).strip()
     返回 {"version": ver, "notes": notes}
```

移植要点：
- **JSON 优先**，但只有「是 object 且含 `version` 键」才采用；否则一律按纯文本处理。
  `[1,2]`、`"abc"`、`{"a":1}` 这些合法 JSON 都走纯文本。
- JSON 数组型 `notes` 用 `\n` 连接，**不**对数组元素做 strip；字符串型 `notes` 会 strip。
- 纯文本分支的 `notes` 会整体 strip（因此末尾空行被去掉，行间空行保留）。
- 首行就是版本号，**不做任何格式校验**；`"v1.2"`、`"abc"`、`"1.2 beta"` 都被原样接受。

### 2.2 `version_key(ver) -> tuple[int, ...]`（第 120–123 行）

```
parts = 用正则 \d+ 从 str(ver or "") 中提取的所有连续数字串
返回 tuple(int(x) for x in parts)  若 parts 非空
否则返回 (0,)
```

- 非数字字符全部是分隔符：`"1.1.1" -> (1,1,1)`；`"v1.2-beta3" -> (1,2,3)`；
  `"1.0.0.4" -> (1,0,0,4)`；`"abc" -> (0,)`；`"" -> (0,)`；`"0" -> (0,)`。
- **潜在陷阱**：`"abc" -> (0,)` 与 `"0" -> (0,)` 相等；`"1..2" -> (1,2)` 与 `"1.2" -> (1,2)` 相等。
  逐字复刻时必须保留这个语义（按数字段切分，空段丢弃）。

### 2.3 `is_newer_version(remote, local) -> bool`（第 126–132 行）

```
r, l = version_key(remote), version_key(local)
n = max(len(r), len(l))
r = r 后面补 (n-len(r)) 个 0
l = l 后面补 (n-len(l)) 个 0
返回 r > l          # 元组字典序比较
```

语义：**只有远程严格大于本地才算有更新**；相同、更旧都返回 `False`。
例：`("1.1.0","1.0")=True`；`("1.0","1.0.0")=False`；`("1.0.0","1.0")=False`；
`("0.9","1.0")=False`；`("1.2","1.1")=True`。
**C++ 实现注意**：元组比较是字典序；补 0 后再比较，等价于「逐段比较，缺失段视为 0」。

### 2.4 本地版本号来源（第 135–145 行）

```
local_manifest_path() = PROGRAM_DIR / "update.wenyi"      # 程序目录，与 data 目录无关
local_version():
    try:  返回 parse_update_manifest(读取该文件, encoding="utf-8")["version"] or "0"
    except: 返回 "0"
```

- 读文件失败（不存在/编码错）→ `"0"`。
- 版本为空字符串 → `"0"`（因为 `m.get("version") or "0"`）。

### 2.5 主程序侧同义实现（`neiwang.py` 第 202–275 行）

`parse_update_manifest` / `version_key` / `is_newer_version` 与更新程序**逐字相同**。
差别在于主程序还做：
```
local_manifest_path() = 主程序目录 / "update.wenyi"
load_local_manifest(): try 解析；except 返回 {"version":"","notes":""}
apply_local_version(): m = load_local_manifest(); 若 m["version"] 非空 -> 全局 APP_VERSION = m["version"]
apply_local_version()   # 模块导入时立即执行
LOCAL_MANIFEST = load_local_manifest()   # 模块级缓存
```
因此**程序运行时显示的版本号 = 本机 `update.wenyi` 第一行**，`"1.1.0"` 只是兜底。
本仓库 `update.wenyi` 首行为 `1.1`，故本机实际 `APP_VERSION == "1.1"`。

---

## 3. 更新三步的精确文件操作

### 3.0 配置读取 `load_config()`（第 148–162 行）

```
cfg = {"update_url": DEFAULT_UPDATE_URL, "update_page": DESKTOP_UPDATE_PAGE,
       "update_repo": DEFAULT_UPDATE_REPO, "update_package": ""}
try:
    data = JSON.parse( 读取 (get_data_dir() / "config.json"), encoding="utf-8" )
    if data 是 object: cfg.update(data)      # 以文件中的键**覆盖/追加**默认值
except: 忽略（保留默认值）
返回 cfg
```

- **读取位置是数据目录**（`NEIWANG_DATA_DIR` 或 `<程序目录>/data`），不是程序目录。
- `config.json` 不存在/不是合法 JSON/不是对象 → 三个默认地址生效，`update_package` 为空。
- 出现的键直接覆盖默认值，**不做类型转换**（例如 `update_url` 为数字时下游用 `str(...)` 兜住）。
- 主程序 `load_config`（第 490–525 行）在缺键时用 `setdefault` 填成同样的四个值，并 `save_config` 落盘。

用到的 4 个 config 字段：`update_url`（检查清单）、`update_page`（发布页）、
`update_repo`（Git 仓库）、`update_package`（ZIP 包地址）。

### 3.1 `perform_update(cfg, log)` 总流程（第 345–379 行）

```
root      = update_root()                       # NEIWANG_UPDATE_ROOT 或 程序目录
data_root = get_data_dir()                      # NEIWANG_DATA_DIR 或 程序目录/data
backup    = root / "customer_god"
target    = root / "update_clone"

输出日志: "1/3 正在把会话内容备份到 customer_god/ ..."
1) backup_customer_data(data_root, backup, log)
   异常 -> {"ok": False, "reason": "备份会话内容失败：{类型名}: {e}"}   （无 path/backup 键）
   正常 -> 日志 "    会话内容已备份：{backup}"

输出日志: "2/3 正在取回新版本（Git 或 ZIP 更新包）..."
2) res = fetch_new_version(cfg, target, log)
   res["ok"] 为假 -> {"ok": False, "reason": res.get("reason","取回新版本失败")}
   正常 -> 日志 "    新版本已就绪：{target}（方式：{'Git' | 'ZIP 更新包'}）"

输出日志: "3/3 正在把会话内容回填到新版本 ..."
3) restore_customer_data(backup, target, log)
   异常 -> {"ok": False, "path": str(target), "backup": str(backup),
            "reason": "回填会话内容失败：{类型名}: {e}"}
   正常 -> 日志 "更新完成，新版本在：{target}"
           返回 {"ok": True, "path": str(target), "backup": str(backup), "method": res["method"]}
```

日志一律经 `log` 回调（默认 `print`；GUI 模式下投递到界面「进度」框）。

### 3.2 第一步：备份 `data/` → `customer_god/`（第 206–212 行）

```
backup_customer_data(data_root, backup, log=print):
    data_root.mkdir(parents=True, exist_ok=True)     # 源目录不存在则**创建空目录**
    backup.mkdir(parents=True, exist_ok=True)        # 备份目录不存在则创建
    shutil.copytree(data_root, backup, dirs_exist_ok=True)
```

精确语义：
- **递归整目录复制**，`dirs_exist_ok=True` 表示目标已存在时**合并覆盖**，不报错、不先清空。
- 文件同名则覆盖（`copy2` 语义，保留时间戳）；备份目录里**多出来的旧文件不会被删除**。
- 源目录里的符号链接等按 `copytree` 默认行为处理（`symlinks=False`：复制链接目标内容）。
- 备份的是**整个 data 目录**（含 `config.json`、群组/聊天/文件索引/转存记录等）。
- `log` 参数被接收但**此函数不调用 log**。

### 3.3 第二步：取回新版本到 `update_clone/`（`fetch_new_version`，第 291–342 行）

```
repo    = str(cfg.get("update_repo") or "").strip()
package = str(cfg.get("update_package") or "").strip()
target  = Path(target)
tried   = []                                  # 失败原因累积列表

if target.exists(): shutil.rmtree(target, ignore_errors=True)   # 无条件先删旧目录

── 方式一：Git ───────────────────────────────────────────────
if repo != "" 且 repo 中不含 "<":                 # "<" 用于过滤占位符地址
    if have_git():                                # shutil.which("git") is not None
        ok, out = run_git_clone(repo, target, log)
        if ok 且 (target/"neiwang.py").is_file():  return {"ok": True, "method": "git"}
        tried.append("Git 方式失败：" + (out 或 "仓库内容不完整（没有找到 neiwang.py）"))
        shutil.rmtree(target, ignore_errors=True)
    else:
        tried.append("本机没有安装 Git，无法用 Git 方式更新")
else:
    tried.append("没有配置 update_repo（Git 仓库地址）")

── 方式二：ZIP 更新包 ─────────────────────────────────────────
if package != "" 且 package 中不含 "<":
    zip_path  = update_root() / "update_package.zip"
    unzip_dir = update_root() / "_update_unzip"
    try:
        download_file(package, zip_path, log)
        shutil.rmtree(unzip_dir, ignore_errors=True)
        unzip_dir.mkdir(parents=True, exist_ok=True)
        log("    正在解压更新包 ...")
        with ZipFile(zip_path) as zf: safe_extract(zf, unzip_dir)
        src = single_root(unzip_dir)              # 只有一层根目录时进入那一层
        target.mkdir(parents=True, exist_ok=True)
        shutil.copytree(src, target, dirs_exist_ok=True)
        if (target/"neiwang.py").is_file(): return {"ok": True, "method": "zip"}
        tried.append("ZIP 更新包里没有找到 neiwang.py（包内容不完整）")
    except Exception as e:
        tried.append(f"ZIP 更新包失败：{type(e).__name__}: {e}")
    finally:
        shutil.rmtree(unzip_dir, ignore_errors=True)   # 无论成败都删除解压临时目录
else:
    tried.append("没有配置 update_package（ZIP 更新包地址）")

── 都失败 ────────────────────────────────────────────────────
return {"ok": False, "reason": "取回新版本失败：\n- " + "\n- ".join(tried)}
```

`run_git_clone(repo, target, log)`（第 272–288 行）：
```
logfile = update_root() / "update_clone.log"
log("    git clone --depth 1 {repo}")
try:
    subprocess.run(["git","clone","--depth","1", repo, str(target)],
                   stdout=logfile(以 "w" 打开, encoding utf-8, errors replace),
                   stderr=STDOUT, timeout=900)
except FileNotFoundError: return (False, "未找到 git 命令：本机没有安装 Git")
except Exception as e:    return (False, f"{type(e).__name__}: {e}")
out = 读回 logfile 文本（失败则 ""）
return (proc.returncode == 0, out.strip()[-400:])     # 只保留末尾 400 字符
```
- **Git 方式直接 clone 到 `update_clone/` 本身**（不经过临时目录）。
- `git` 输出**不经过管道**，直接写文件，避免管道阻塞；日志文件每次以 `"w"` 截断重写。
- 成功后仍要做「`neiwang.py` 存在」校验；`git clone` 成功但缺主程序 = 失败，并**删除 target**。

`download_file(url, dest, log, timeout=300)`（第 227–251 行）：
```
req = Request(url, headers={"User-Agent": f"NeiWangUpdater/{local_version()}"})
dest.parent.mkdir(parents=True, exist_ok=True)
log("    正在下载：{url}")
last_mark = -1; got = 0
打开 urlopen(req, timeout=300) 与 目标文件 "wb"
total = int(响应头 Content-Length or 0)
循环: chunk = 读 64*1024 字节；空则结束
      写入文件；got += len(chunk)
      mark = got // (2*1024*1024)                # 每 2MB 一个刻度
      if mark != last_mark:
          last_mark = mark
          total 存在 -> log("    已下载 {got//1024} KB / {total//1024} KB")
          否则       -> log("    已下载 {got//1024} KB")
log("    下载完成：{dest}（{got//1024} KB）")
```
- 进度**只在跨过 2MB 边界时**打印一次（首块必然打印，因为 `mark=0 != -1`）。
- 无 `Content-Length` 时打印不带总数的版本。
- 异常（HTTP 错误、超时、磁盘错误）**向上抛出**，由 `fetch_new_version` 的 `except` 收成
  `"ZIP 更新包失败：{类型名}: {e}"`。

`safe_extract(zf, dest)`（第 254–261 行）：
```
dest = Path(dest).resolve()
对 zf.infolist() 中每个成员：
    name = 成员文件名中的 "\\" 全部替换为 "/"
    若 name 以 "/" 开头             -> raise ValueError("压缩包里含有不安全的路径，已中止：{原始文件名}")
    若 re.match(r"^[A-Za-z]:", name) -> raise ValueError(同上)
    若 ".." 是 name.split("/") 的元素 -> raise ValueError(同上)
    zf.extract(member, dest)
```
- 拒绝：绝对路径（`/` 开头）、盘符路径（`C:`）、路径段恰好等于 `..`。
- **注意**：成员名里含 `..`（如 `a..b`）不会被拒；检查的是按 `/` 切分后的**整段等于** `..`。
- 抛 `ValueError` 后已解压的成员保留在临时目录，但 `finally` 会删除整个临时目录。

`single_root(folder)`（第 264–269 行）：
```
items = list(folder.iterdir())
若 len(items)==1 且 items[0] 是目录 -> 返回 items[0]
否则 -> 返回 folder 本身
```
- **空目录**：返回 `folder` 本身（`len==0`）。
- 这正是 GitHub ZIP（单层 `<repo>-main/`）与「平铺 ZIP」两种包都能用的原因。
- 多层嵌套时只剥一层。

ZIP 路径的覆盖/合并规则：`copytree(src, target, dirs_exist_ok=True)`——因为
`fetch_new_version` 开头已删过 `target`，此处实际是全新创建；若 `single_root` 返回解压目录
本身而解压目录里有多余的边角文件，它们也会一并被复制过去。

**失败分支全集（`tried` 文案，逐字）**
1. `没有配置 update_repo（Git 仓库地址）`
2. `本机没有安装 Git，无法用 Git 方式更新`
3. `Git 方式失败：{git 输出末尾 400 字符}` 或 `Git 方式失败：仓库内容不完整（没有找到 neiwang.py）`
4. `没有配置 update_package（ZIP 更新包地址）`
5. `ZIP 更新包失败：{异常类型名}: {异常信息}`
6. `ZIP 更新包里没有找到 neiwang.py（包内容不完整）`

最终 `reason` 形如（前缀固定 `取回新版本失败：\n- `，多条用 `\n- ` 连接）：
```
取回新版本失败：
- 本机没有安装 Git，无法用 Git 方式更新
- 没有配置 update_package（ZIP 更新包地址）
```

**「是否尝试 Git/ZIP」的判定**：`repo` / `package` 非空且**不含字符 `<`**。
（`<...>` 占位符地址会被跳过，并把对应「没有配置」文案写进 `tried`。）

### 3.4 第三步：回填 `customer_god/` → `update_clone/data/`（第 215–224 行）

```
restore_customer_data(backup, target, log=print):
    newdata = Path(target) / "data"
    newdata.mkdir(parents=True, exist_ok=True)          # target 不存在时会被创建
    对 Path(backup).iterdir() 中每个 item:
        dst = newdata / item.name
        目录 -> shutil.copytree(item, dst, dirs_exist_ok=True)   # 合并覆盖
        文件 -> shutil.copy2(item, dst)                          # 覆盖同名文件
```

精确语义：
- **只遍历备份目录的第一层**，逐项递归复制；目录用 `copytree(dirs_exist_ok=True)`，
  文件用 `copy2`（覆盖）。
- `backup` 目录不存在时 `iterdir()` 抛 `FileNotFoundError` → 被 `perform_update` 捕获成
  `回填会话内容失败：FileNotFoundError: ...`。（正常流程第 1 步已创建 backup。）
- **因为备份的是整个 `data/`（含 `config.json`），回填会把用户旧配置覆盖到
  `update_clone/data/config.json`**——即新版本会沿用用户的配置与全部会话内容。
- `log` 参数被接收但**此函数不调用 log**。

### 3.5 界面行为（`UpdaterApp`，第 418–652 行）

窗口：标题 `内部网 更新程序`；几何 `560x460`；最小 `500x400`；背景 `#ffffff`。

配色常量：`BG=#ffffff`、`PANEL=#f7f7f7`、`TEXT=#191919`、`SUB=#9a9a9a`、
`ACCENT=#07c160`、`ACCENT_HV=#06ad56`、`BORDER=#e5e5e5`。

控件（自上而下）：
1. 标题 `内部网 更新程序`（Microsoft YaHei UI 14 bold）
2. 状态标签，初始文本 `正在检查更新 ...`（11 号，wraplength=500，左对齐）
3. 版本标签，初始 `当前版本 v{local_version()}`（9 号，`SUB` 色）
4. 更新内容文本框（高 6，只读，初始插入 `（暂时没有更新内容）`，`wrap="word"`）
5. 小标题 `进度`（9 号）
6. 进度日志文本框（高 8，只读，Consolas 9，`wrap="word"`）
7. 按钮行（`side="left"`，间距 6）：`立即更新`(主色) `打开新版本文件夹` `启动新版本` `打开发布页` `关闭`

按钮初始状态：`立即更新` / `打开新版本文件夹` / `启动新版本` / `打开发布页` 全部 `disabled`；
`关闭` 可用。

调度：`after(100, _poll)` 开始每 100ms 消费线程队列；`after(150, _start)` 开始检查。
`_post(fn)` 把回调塞进 `queue.Queue`，由主线程 `_poll` 执行——**所有界面更新只在主线程发生**。

`_start()`：`mode=="update"` → 直接 `_begin_update()`；否则状态置
`正在检查更新 ...` 并起 daemon 线程 `check`。

`_on_check(res)` 分支：

| status | manual / update 模式 | auto 模式 |
| --- | --- | --- |
| `update` | 状态 `发现新版本 —— v{remote}（当前 v{local}）`；显示 notes；日志两行：`远程版本：v{remote}` 与 `点「立即更新」开始：先备份会话内容，再取回新版本，最后把会话内容回填到新版本。`；启用 `立即更新`；若 `_page()` 非空则启用 `打开发布页` | 先 `deiconify()+lift()` 再同上 |
| `current` | 状态 `已经是最新版本（v{local}）`；日志 `远程版本与本地相同或更旧，不需要更新。` | `destroy()` 直接退出，不显示 |
| `offline` | 状态 `无法连接更新服务器`；日志 `res.reason`（默认 `没有网络或地址不可用`）；再日志 `请检查网络连接，或确认 config.json 里的 update_url 是否正确。`；若 `_page()` 非空则启用 `打开发布页` | `destroy()` 直接退出 |

版本标签在 `_on_check` 开头统一刷新为 `当前版本 v{res["local"] 或 local_version()}`。

`_page()`：`str(cfg.get("update_page") or DESKTOP_UPDATE_PAGE).strip()`。
`_open_page()`：若非空，`webbrowser.open(page)`，异常静默。

`_begin_update()`：`_busy` 或 `_done` 为真则直接返回；置 `_busy=True`；auto 模式
`deiconify()+lift()`；禁用 `立即更新` 与 `打开发布页`；状态 `正在更新 ...`；日志 `开始更新。`；
起 daemon 线程 `update` 调用 `perform_update(cfg, log=...)`（每条日志经 `_post` 投递到界面）。

`_on_update_done(res)`：
- 成功：`_done=True`；`_new_dir=res["path"]`；状态 `更新完成，请到新版本目录使用`；
  日志 `新版本目录：{path}` 与 `会话内容备份：{backup}`；启用 `打开新版本文件夹` 与 `启动新版本`；
  `关闭` 按钮文字改为 `关闭`（原地重设，无实际变化）；弹出**信息框**（见下）。
- 失败：状态 `更新失败`；日志写 `res["reason"]`；重新启用 `立即更新`；
  弹出**警告框**标题 `更新失败`、正文 `res["reason"]`（默认 `未知错误`）。`_done` 保持 False
  → 用户可改配置后重试，但 `_busy` 已复位。

成功信息框（标题 `更新完成`，逐字）：
```
新版本已就绪：
{path}

你的所有会话内容（群组、聊天、文件）已备份到 customer_god/，并已回填进新版本的 data/ 目录。

请关闭主程序，然后到上面的目录双击「启动.bat」使用新版本
（也可以直接点下面的「启动新版本」）。
```

`_open_new_dir()`：`_new_dir` 非空时 `open_folder(_new_dir)`。
`open_folder(path)`：Windows 用 `os.startfile(path)`；macOS `open`；其它 `xdg-open`；异常静默。

`launch_new_version(new_dir)`（第 398–412 行）：
```
bat = new_dir / "启动.bat"
若 os.name=="nt" 且 bat 是文件 -> os.startfile(bat); return True
main_py = new_dir / "neiwang.py"
若 main_py 是文件 -> Popen([sys.executable 或 "python", main_py], cwd=new_dir); return True
异常 -> return False
都不满足 -> return False
```
`_launch_new()` 在返回 False 时弹警告标题 `启动失败`，正文
`没能自动启动，请手动到下面目录双击「启动.bat」：\n{new_dir}`。

`更新.bat`（逐字行为）：
```
@echo off
chcp 65001 >nul
title 内部网 更新程序
cd /d "%~dp0"
where pythonw >nul 2>nul
if %errorlevel%==0 ( start "" pythonw neiwang_update.py ) else ( python neiwang_update.py )
```

---

## 4. 主程序 `neiwang.py` 的更新检查流程

### 4.1 检查时机

- **每次启动自动检查**：`App.__init__` 中 `self.core.start()` 之后立即
  `self.core.check_update(manual=False)`（第 2662 行）。窗口标题为
  `内部网 v{APP_VERSION}  ·  局域网群组`。
- **手动检查**：工具栏按钮 `检查更新` → `_on_manual_update()`：
  ```
  若 self._manually_check 为假:
      self._manually_check = True
      self._update_shown = False      # 手动检查时允许再次弹出提醒
      self.core.check_update(manual=True)
  ```
  （`manual` 参数在 `check_update` 内部**未被使用**，但主程序侧的这两个状态位必须复刻。）
- `check_update` 在 daemon 线程 `update-check` 中执行，完成后 `_emit("update", payload)`
  推入事件队列；GUI 的 `_poll_events`（每 100ms）取到 `kind == "update"` 时调用
  `_on_update_result(ev[1])`（第 3758–3759 行）。

### 4.2 `check_update` 的请求与判定（第 2443–2479 行）

```
url = str(cfg["update_url"]).strip()
若 url 为空 -> _emit("update", {"ok": False, "reason": "no-url"}); 返回
req = Request(url, headers={"User-Agent": f"NeiWangLAN/{APP_VERSION}"})
打开 URL，timeout = 8 秒（硬编码，与更新程序的 CHECK_TIMEOUT 一致）
读最多 65536 字节，按 utf-8 errors="replace" 解码
manifest = parse_update_manifest(data)
remote, notes = manifest["version"], manifest["notes"]
若 remote 非空 且 is_newer_version(remote, APP_VERSION):
    _emit("update", {"ok": True, "found": True, "remote": remote, "notes": notes,
                     "local": APP_VERSION, "page": cfg["update_page"],
                     "repo": cfg["update_repo"], "package": cfg["update_package"]})
否则:
    _emit("update", {"ok": True, "found": False})        # 静默
任何异常:
    _emit("update", {"ok": False, "reason": "offline"})  # 静默
```

事件载荷字段：`ok`、`found`、`remote`、`notes`、`local`、`page`、`repo`、`package`；
失败时为 `{"ok":False,"reason":"no-url"|"offline"}`。

### 4.3 静默条件（**不弹任何窗口、不打印任何提示**）

只要不满足 `res["ok"] and res["found"]`，`_on_update_result` **什么都不做**。即：
- `res["ok"] == False`（`no-url` 或 `offline`：没配置地址 / 没联网 / 服务器无响应）；
- `res["ok"] == True` 且 `found == False`（远程版本号与本地**相同或更旧**，或清单里 `version` 为空）。

### 4.4 `_on_update_result` 弹窗逻辑（第 3940–3971 行）

```
self._manually_check = False
若 self._update_shown: return                      # 本次会话已提醒过，不重复弹
若 res.ok and res.found:
    self._update_shown = True
    remote = res["remote"] 或 "?"
    local  = res["local"]  或 APP_VERSION
    notes  = str(res["notes"] or "").strip()
    page   = res["page"] or self.cfg["update_page"]
    msg  = f"新版本 —— v{remote}\n\n"
    if notes: msg += f"更新内容：\n{notes}\n\n"
    msg += (f"（当前版本 v{local}）\n\n"
            f"「是」立即更新：启动同目录下的独立更新程序 neiwang_update.py，\n"
            f"它会先把你的所有会话内容备份到 customer_god 文件夹，\n"
            f"再取回新版本并把你的会话内容接着用；\n"
            f"「否」打开更新页面手动下载；「取消」稍后再说。")
    answer = askyesnocancel("发现新版本", msg, parent=主窗口)
    if answer is True:      _start_update()                    # 「是」
    elif answer is False 且 page 非空: webbrowser.open(page)   # 「否」；page 为空则什么都不做
    else:                   self._update_shown = False         # 「取消」或关窗：允许下次再提醒
```

`_start_update()`（第 3973–4002 行）：
```
若 self._updating: return
updater = updater_path()                       # 主程序目录 / "neiwang_update.py"
若 updater 不是文件:
    showwarning("找不到更新程序",
        "同目录下没有独立更新程序：\n{updater}\n\n请把 neiwang_update.py 与本程序放在同一个文件夹里，"
        "或直接运行「更新.bat」。", parent=主窗口)
    return                                     # 注意：不置 _updating
try:
    Popen([sys.executable 或 "python", str(updater), "--update"], cwd=updater.parent)
except e:
    showwarning("启动更新程序失败", f"{类型名}: {e}", parent=主窗口); return
self._updating = True
追加日志 "[{时间}] 系统: 已启动独立更新程序 neiwang_update.py --update"
showinfo("已启动更新程序",
    "已启动独立更新程序：\n{updater}\n\n"
    "请在它弹出的窗口里完成更新（会先把你的会话内容备份到 customer_god/，\n"
    "再把新版本取回并接着用这些会话内容）。\n\n"
    "更新完成后关闭本程序，到新版本目录双击「启动.bat」即可。", parent=主窗口)
```

三个按钮行为汇总：

| 按钮 | `answer` | 行为 |
| --- | --- | --- |
| 是 | `True` | `_start_update()`：以 `--update` 调起同目录 `neiwang_update.py`（cwd = 该文件所在目录），`_updating=True`，写日志，弹「已启动更新程序」信息框 |
| 否 | `False` | 用系统默认浏览器打开 `page`（后台静默）；`page` 为空则无动作；`_update_shown` 保持 `True`（本次会话不再提醒） |
| 取消 / 关闭对话框 | `None` | `_update_shown = False` → 手动检查或下次启动仍会提醒 |

其它复刻要点：
- `_update_shown` 初始 `False`、`_manually_check` 初始 `False`、`_updating` 初始 `False`。
- 主程序**不含任何更新执行代码**：无 `clone_update`、无 `restore_customer_data`；
  实际更新全部在独立程序里完成。
- `check_update` 的 `timeout=8` 与 65536 字节读取上限是与更新程序共享的硬约束。

---

## 5. 四个测试文件的行为清单（逐条断言）

**断言总数：198 条 `ok(...)` 调用点**——`test_protocol.py` 55、`test_net.py` 75、
`test_ui_net.py` 27、`test_gui_smoke.py` 48。其中少数位于 if/try 分支内（互斥），
运行时实际执行条数可略少（下表逐条标注）。

每个文件共同的测试骨架（C++ 等价测试也必须复刻）：
```
PASS = []                       # 记录 (名字, 布尔)
ok(name, cond, extra="")        # 追加并打印 "PASS {name}[  {extra}]" / "FAIL ..."
wait_for(fn, timeout, step)     # 轮询直到 fn 为真或超时；fn 抛异常按 False 处理
结尾: failed = [名字 | 假]; 打印 "结果：{通过}/{总数} 通过"; 若有失败再打印 "失败项：a, b"
      sys.exit(1 if failed else 0)
```

---

### 5.1 `test_protocol.py` —— 无头协议测试（4 台模拟电脑），55 条

**环境变量（模块导入 `neiwang` 之前设置）**：
`NEIWANG_PRES_INTERVAL=1`、`NEIWANG_PRES_TTL=3`、`NEIWANG_OFFER_WAIT=6`、
`NEIWANG_UDP_PORT=45679`、`NEIWANG_TCP_PORT=45631`、`NEIWANG_VOTE_NEED=1`、
`NEIWANG_VOTE_WAIT=8`、`NEIWANG_SCAN_WINDOW=1`、`NEIWANG_NET_ANNOUNCE=2`、
`NEIWANG_NET_REQ_REBROADCAST=2`。

**临时目录**：`<脚本目录>/_testdata`，脚本启动时若存在先整个删除再创建；
四台机器分别用 `_testdata/A`、`/B`、`/C`、`/D` 作为 `data_root`（`PeerCore(cfg,q,data_root=root)`），
发送用文件放 `_testdata/A_send/`。`UDP` 常量为 `45679`；四个 TCP 口 `45631/45632/45633/45634`。
`make_core` 的 cfg 里 `update_url="http://127.0.0.1:1/none"`、`update_page=""`。

| # | 行 | 断言名 | 断言内容 |
| --- | --- | --- | --- |
| 1 | 120 | A 自动建立内网 | 15s 内 `coreA.in_net()` 为真 |
| 2 | 122 | B 自动加入同一内网 | 20s 内 `coreB.in_net()` 且 `coreB.net["nid"] == A 的 nid` |
| 3 | 123 | C 自动加入同一内网 | 同上（20s） |
| 4 | 124 | D 自动加入同一内网 | 同上（20s） |
| 5 | 125 | A 的成员登记表记录了 B/C/D 的物理地址 | 15s 内 `{"UID-B","UID-C","UID-D"} ⊆ set(coreA.net_members)` |
| 6 | 127 | D 的登记表里有 A | 10s 内 `"UID-A" in coreD.net_members` |
| 7 | 134 | 创建群组 | `coreA.create_group("qqq")["gid"] in coreA.groups` |
| 8 | 135 | 多群加入 | `gid in coreB.groups and gid in coreC.groups` |
| 9 | 139 | 群聊文字接收（B） | 10s 内 B 队列出现 `("chat", {text:"大家好，我是 AA"})` |
| 10 | 141 | 群聊文字接收（C） | 同上（C） |
| 11 | 151 | B 自动下载(在线直连) | 20s 内 B 的 index 出现 name==`你好.txt` |
| 12 | 152 | C 自动下载(在线直连) | 同上（C） |
| 13 | 154 | B 文件内容一致（SHA256） | *if fid 存在*：`sha256(B.cache_dir()/fid) == sha256(源文件)` |
| 14 | 158 | B 系统通知格式 | 10s 内 B 出现 `("notify", …, "「qqq」群「AA」的「你好.txt」下载完毕")` |
| 15 | 159 | B 日志出现「AA」发送文件 | 10s 内 B 出现 `("log", "「AA」在群「qqq」发送了文件「你好.txt」")` |
| 16 | 164 | 改名广播日志 | 10s 内 B 出现含 `「AA」更名为「AAA改」` 的 log |
| 17 | 168 | A 已进入投票改名模式 | `coreA.rename_needs_vote()` 为真 |
| 18 | 171/173 | 第二次直接改名被拒绝 | `coreA.rename("绕过投票")` 抛 `PermissionError`（抛了 → 真） |
| 19 | 176 | B 收到改名投票请求 | 10s 内 B 出现 `("rename_request", {pid: pid1})` |
| 20 | 178 | C 收到改名投票请求 | 同上（C） |
| 21 | 180 | 投票请求内容正确（old/new） | C 队列中存在 `old=="AAA改" and new=="AA2"` 的 rename_request |
| 22 | 187 | 同意票不足 -> 改名失败 | `RENAME_VOTE_WAIT+8` 内 A 出现 `("rename_vote_done", pid1, False)` |
| 23 | 190 | 失败后名字保持不变 | `coreA.my_name == "AAA改"` |
| 24 | 194 | 二次发起后 B 再收到请求 | 10s 内 B 出现 pid2 的 rename_request |
| 25 | 196 | 二次发起后 C 再收到请求 | 同上（C） |
| 26 | 201 | 超过票数 -> 改名成功 | 10s 内 A 出现 `("rename_vote_done", pid2, True)`（B 重复投票被忽略、C 同意） |
| 27 | 203 | A 的名字已更新为 AA2 | `coreA.my_name == "AA2"` |
| 28 | 204 | B 看到投票通过的改名日志 | 10s 内 B 出现含 `「AAA改」更名为「AA2」` 且含 `投票通过` 的 log |
| 29 | 210 | B 收到 A 的私聊 | 10s 内 B 出现 `("dm", {from_uid:"UID-A", text:"B 你好，这是私聊", self 假})` |
| 30 | 214 | 私聊发送人名字正确 | B 队列中存在 `from_name == "AA2"` 的 dm |
| 31 | 216 | A 本机回显自己的私聊 | 10s 内 A 出现 `("dm", {self:True, to_uid:"UID-B"})` |
| 32 | 220 | A 收到 B 的私聊回复 | 10s 内 A 出现 `{from_uid:"UID-B", text:"AA2 你好，收到"}` |
| 33 | 223 | C 收不到发给 B 的私聊 | C 队列中**不存在**非 self 的 dm 事件 |
| 34 | 224 | 发给自己被拒绝 | `coreA.send_dm("UID-A", …) is False` |
| 35 | 228 | 转存加解密往返正确 | `secret_decrypt(secret_encrypt("完整校验测试", k), k) == "完整校验测试"`（k=32 随机字节） |
| 36 | 233 | B 下线 | 8s 内 `not coreA.is_online("UID-B")`（B 已 `stop()`） |
| 37 | 234 | D 下线（内网成员离线） | 8s 内 `not coreA.is_online("UID-D")` |
| 38 | 244 | A 发起离线转存(dm_sent) | 12s 内 A 出现 `("dm_sent", {to_uid:"UID-D"})` |
| 39 | 245 | 转存成功且非直发 | `sent.ok is True and sent.direct is False` |
| 40 | 246 | C 已持有转存 | 6s 内 `bool(coreC._secrets)` |
| 41 | 248 | C 端为密文（不含原文） | `rec.to_uid=="UID-D"`、序列化后不含明文、`blob` 与 `key` 均非空 |
| 42 | 251 | C 无感（无任何事件） | C 队列中没有 `dm` 或 `dm_sent` 事件 |
| 43 | 252 | C 磁盘持久化了转存 | `(coreC.data_root/"secret_relay.json").exists()` |
| 44 | 256 | D 上线后收到转存私聊 | 20s 内 D 出现 `{from_uid:"UID-A", text:"D 离线时的保密消息", relayed:真}` |
| 45 | 260 | D 校验通过后 C 删除转存 | 10s 内 `not coreC._secrets` |
| 46 | 261 | C 磁盘转存已清空 | 6s 内文件不存在，或内容 JSON 解析后 == `{}` |
| 47 | 280 | C 先拿到离线测试文件 | 20s 内 C 的 index 出现 `离线文件.bin`（文件 300KB 随机） |
| 48 | 285 | A 被判定离线 | 8s 内 `not coreB.is_online("UID-A")`（先 `coreA.stop()` 再 `sleep(4)`） |
| 49 | 294 | B 从公共询问自动下载(离线回退) | 25s 内 B 的 index 出现 `离线文件.bin`（先删 B 的缓存与 index 项并 `_save_index()`，再 `coreB._sync_req(gid)`） |
| 50 | 296 | B 离线回退文件内容一致 | *if fid 存在*：SHA256 与源文件相同 |
| 51 | 298 | 离线回退通知显示原作者 | 15s 内 B 出现含 `「AA2」的「离线文件.bin」下载完毕` 的 notify |
| 52 | 300 | B 离线回退通知不含转发者 CC | B 队列中**不存在**含 `「CC」的「离线文件.bin」下载完毕` 的 notify |
| 53 | 302 | B 日志出现公共询问 | 15s 内 B 出现含 `公共询问文件` 的 log |
| 54 | 306 | 退出群组 | `gid not in coreB.groups`（`coreB.leave_group(gid)` 之后） |
| 55 | 311–312 | — | 收尾循环**空转**，只是形式上的 `for name, cond in PASS: pass`，不产生断言 |

（表中第 1–9 条对应 `ok` 第 85–93 行，第 10 条对应第 94 行。
「第二次直接改名被拒绝」「B 文件内容一致」「B 离线回退文件内容一致」各有 if 守卫，
未执行时不产生断言；故 55 个调用点对应 54 个逻辑条目或更少。）

结尾：`coreB.stop(); coreC.stop(); sys.exit(1 if failed else 0)`。
（第 267 行还重建了 B 实例 `coreB2` 并重新入群，供第 49–53 条使用。）

---

### 5.2 `test_net.py` —— 内网准入层测试，75 条

**环境变量（导入 `neiwang` 前）**：`NEIWANG_UDP_PORT=45695`、`NEIWANG_PRES_INTERVAL=1`、
`NEIWANG_PRES_TTL=5`、`NEIWANG_SCAN_WINDOW=1`、`NEIWANG_NET_ANNOUNCE=2`、
`NEIWANG_NET_REQ_REBROADCAST=2`、`NEIWANG_NET_VOTE_WINDOW=12`、
`NEIWANG_NET_VOTE_CHANGE=5`、`NEIWANG_NET_NEARBY_TTL=6`。

**临时目录**：`<脚本目录>/_testdata_net`，先删后建；每个模拟机一个子目录
（`A`/`B`/`X`/`Y`/`Z`/`W`/`V`/`N`）；TCP 口 `45661/45662/45670/45671/45672/45673/45674/45699`。

| # | 行 | 断言名 | 断言内容 |
| --- | --- | --- | --- |
| 1–9 | 85–93 | 投票门槛表 | `M.net_vote_need(n)[0]`：0→0、1→0、5→0、6→6、7→7、8→3、9→4、12→5、15→6 |
| 10 | 94 | 规则说明里带上人数与票数 | `"20 人" in net_vote_need(20)[1]` 且 `str(20//3+1)` 也在其中 |
| 11 | 102 | 启动后自动建立内网 | 12s 内 `coreA.in_net()` |
| 12 | 104 | 发起者已登记为成员 | `net_view()["members"]` 长度 1 且 `members[0]["mac"]=="AABBCC000001"` |
| 13 | 105 | 登记表记录物理地址（MAC 形式） | `M.fmt_mac("AABBCC000001") == "AA-BB-CC-00-00-01"` |
| 14 | 106 | 发起者角色正确 | `members[0]["founder"] is True` 且 `net_view()["is_founder"]` |
| 15 | 107 | 落盘了 net.json | `(coreA.data_root/"net.json").exists()` |
| 16 | 117 | B 发现附近内网 | 12s 内 `nid in coreB.net_nearby` |
| 17 | 118 | B 自动申请并加入（免投票） | 15s 内 `coreB.in_net()` |
| 18 | 119 | B 加入的是同一个内网 | `coreB.net["nid"] == nid` |
| 19 | 120 | A 的登记表出现 B 的物理地址 | 12s 内 `"AABBCC000002" in coreA.net_members` 且成员数 == 2 |
| 20 | 122 | B 的登记表含 A 与 B | 12s 内 `set(coreB.net_members) == {"AABBCC000001","AABBCC000002"}` |
| 21 | 124 | B 记录到 A 的名字 | `coreB.net_members["AABBCC000001"]["name"] == "AA"` |
| 22 | 125 | 加入后在公共日志里可见 | 10s 内 B 出现含 `加入了内网` 的 log |
| 23 | 135 | A 内网变成 6 人 | 手工加 4 个虚拟成员（`V1RT00000001..4`，端口 40000–40003）后 `len(coreA.net_members)==6` |
| 24 | 141 | X 提交了入网申请 | 15s 内 `coreX.net_applying is not None` |
| 25 | 143 | X 的申请门槛为全票 | 10s 内 `len(net_applying["voters"]) == 6` |
| 26 | 146 | A 那边收到 X 的申请 | 10s 内 `reqX in coreA.net_reqs` |
| 27 | 148 | A 侧门槛同样为 6 票 | `coreA._req_progress(coreA.net_reqs[reqX])["need"] == 6` |
| 28 | 153 | 只有 3 票时不能通过 | 投 3 张同意后 `coreX.net_applying is not None and not coreX.in_net()` |
| 29 | 155 | A 侧显示 3/6 票 | `_req_progress(...)["approvals"] == 3` |
| 30 | 161 | 6 票齐全 -> 通过并加入 | 15s 内 `coreX.in_net()`（第 4 张虚拟票 + `coreB.net_cast_vote(reqX,True)` + `coreA.net_cast_vote(reqX,True)`） |
| 31 | 162 | X 成为内网成员 | `"AABBCC000010" in coreA.net_members` |
| 32 | 163 | X 收到通过提示 | 10s 内 X 出现 `("net_result", True)` |
| 33 | 165 | X 的登记表含 6+1 人 | `len(coreX.net_members) >= 7` |
| 34 | 173 | Y 提交申请 | 15s 内 `coreY.net_applying is not None` |
| 35 | 175 | A 收到 Y 的申请 | 10s 内 `reqY in coreA.net_reqs` |
| 36 | 177 | 出现反对票 -> Y 的申请失败 | 12s 内 `coreY.net_applying is None`（投 1 张反对） |
| 37 | 178 | Y 没有加入内网 | `not coreY.in_net()` |
| 38 | 179 | Y 收到失败原因（无法全票通过） | 10s 内 Y 出现 `("net_result", False, 含"全票"的原因)` |
| 39 | 181 | 失败者不在登记表里 | `"AABBCC000011" not in coreA.net_members` |
| 40 | 194 | A 内网变成 8 人 | 循环补虚拟成员 `V2RT…` 直到成员数正好 8 |
| 41 | 200 | Z 提交申请 | 15s 内 `coreZ.net_applying is not None` |
| 42 | 202 | Z 的申请门槛为 3 票 | 10s 内 `len(net_applying["voters"]) == 8` |
| 43 | 204 | A 收到 Z 的申请 | 10s 内 `reqZ in coreA.net_reqs` |
| 44 | 206 | A 侧门槛 = 3 票（超过 1/3） | `_req_progress(...)["need"] == 3` |
| 45 | 210 | 弃权不计入同意票 | 投 `approve=None` 后 `_req_progress(...)["abstains"] == 1` |
| 46 | 211 | 弃权后仍未通过 | `not coreZ.in_net()` |
| 47 | 215 | 2 票仍不够 | 再投 2 张同意后 `not coreZ.in_net()` |
| 48 | 217 | 3 票 > 1/3 -> Z 加入成功 | 12s 内 `coreZ.in_net()` |
| 49 | 218 | Z 在登记表中 | `"AABBCC000012" in coreA.net_members` |
| 50 | 226 | W 提交申请 | 15s 内 `coreW.net_applying is not None` |
| 51 | 228 | A 收到 W 的申请 | 10s 内 `reqW in coreA.net_reqs` |
| 52 | 230 | A 投了同意 | `coreA.net_my_votes[reqW]["approve"] is True` |
| 53 | 232 | 5 秒内改票为弃权成功 | 改投 `None` 后 `net_my_votes[reqW]["approve"] is None` |
| 54 | 234 | 再次改为同意 | 再投 `True` 后 `is True` |
| 55 | 238/240 | 超过 5 秒后不能再改票 | `sleep(5.5)` 后 `net_cast_vote(reqW,None)` 抛 `RuntimeError`，且消息含 `不能再改票`（互斥分支，2 处调用点） |
| 56 | 248 | V 提交申请 | 15s 内 `coreV.net_applying is not None` |
| 57 | 250 | A 收到 V 的申请 | 10s 内 `reqV in coreA.net_reqs` |
| 58 | 252 | 票数不够时不通过 | 投 1 张同意后 `not coreV.in_net()` |
| 59 | 253 | 超过有效期后申请结束 | 30s 内 `coreV.net_applying is None and not coreV.in_net()` |
| 60 | 255 | 失败原因是投票超时 | 10s 内 V 出现 `("net_result", False)` |
| 61 | 257 | 超时者没有加入内网 | `"AABBCC000014" not in coreA.net_members` |
| 62 | 264/266 | 未入网禁止建群 | 未启动的 N 调 `create_group` 抛 `PermissionError`，消息含 `内网`（互斥，2 处调用点） |
| 63 | 269/271 | 未入网禁止加群 | 调 `join_group("ABCDEF","某群")` 抛 `PermissionError`（互斥，2 处调用点） |
| 64 | 276 | B 已退出内网 | `coreB.net_leave()` 后 `not coreB.in_net()` |
| 65 | 277 | 退出会把自己从登记表移除 | 10s 内 `"AABBCC000002" not in coreA.net_members` |
| 66 | 279 | 退出后不会被自动加回同一个内网 | `sleep(4)` 后仍 `not coreB.in_net()` |
| 67 | 280 | 退出后仍能扫描到该内网 | 12s 内 `nid in coreB.net_nearby` |
| 68 | 282 | 手动重新申请已提交 | `net_apply_join(nid)` 后 `coreB.net_applying is not None` |
| 69 | 284 | A 收到重新申请 | 12s 内 `reqB2 in coreA.net_reqs` |
| 70 | 288 | 票数不够时进不来 | `not coreB.in_net()` |
| 71 | 294 | 票数够了 -> 重新加入成功 | 15s 内 `coreB.in_net()` |
| 72 | 295 | 重新加入后登记表里又有 B | 10s 内 `"AABBCC000002" in coreA.net_members` |

（表中 1–9 计 9 条、10 计 1 条，故前 10 条对应 `ok` 第 85–94 行。
含互斥分支的 3 组共 6 处调用点，故 75 个调用点对应 72 个「逻辑条目」。）

收尾：`for c in (coreA, coreB, coreX, coreY, coreZ, coreW, coreV): c.stop()`（异常吞掉），
再统计失败并 `sys.exit`。

---

### 5.3 `test_ui_net.py` —— 界面控件树断言（Tkinter，无肉眼看图），27 条

**环境变量（`import neiwang` 前）**：`NEIWANG_DATA_DIR=<脚本目录>/_testdata_ui`、
`NEIWANG_UDP_PORT=45751`、`NEIWANG_TCP_PORT=45761`、`NEIWANG_SCAN_WINDOW=1`、
`NEIWANG_NET_ANNOUNCE=2`、`NEIWANG_NET_VOTE_CHANGE=300`。

**临时目录**：`<脚本目录>/_testdata_ui`（`shutil.rmtree` 后重建，与 `NEIWANG_DATA_DIR` 一致）。
界面直接在**进程内同一解释器**创建：`root = tk.Tk(); app = M.App(root); root.update()`。

两个控件树辅助函数（C++ 等价测试需实现同类遍历）：
- `texts(widget)`：递归收集控件树中所有非空 `cget("text")` 字符串；
- `find_button(widget, label)`：按 `winfo_class()=="Button"` 且 `text==label` 递归查找。

| # | 行 | 断言名 | 断言内容 |
| --- | --- | --- | --- |
| 1 | 71 | 导航含聊天/内网/投票/更多/日志 | `set(app.rail_btns) == {"chat","net","vote","more","log"}` |
| 2 | 73 | 投票图标有小红点控件 | `hasattr(app, "vote_dot")` |
| 3 | 74 | 未入网时聊天页显示内网引导页 | `(switch_nav("chat"), update(), app.view_netgate.winfo_ismapped())[-1]` 为真 |
| 4 | 82 | 自动建立内网成功 | 20s 内（轮询 `root.update()`）`app.core.in_net()` |
| 5 | 87 | 没有待投票时没有小红点 | `not app.vote_dot.winfo_ismapped()` |
| 6 | 88 | 无待投票时点图标弹「内容为空」提示 | `_show_vote_popup()` 后 `_vote_popup_body` 的文本合集含 `暂无待我投票` |
| 7 | 108 | 收到入网申请后出现小红点 | 构造 6 人内网 + `_on_net_join_req(req_id="ui-req-1", mac="F1E2D3C4B5A6", name="赵六", ip="192.168.1.88", tcp_port=45431)` 后 `_update_vote_badge()`，`vote_dot` 可见 |
| 8 | 115 | 内网页列出成员登记表 | `switch_nav("net")` 后 `len(net_tree` 行 `) == 6` |
| 9 | 117 | 我的物理地址按可读形式显示 | 存在含 `（我）` 的行，其第 2 列 `== M.fmt_mac(app.core.uid)` 且 `!= "未知"` |
| 10 | 120 | 登记表有内网 IP 列 | 第 1 行第 3 列含 `192.168`，或该列为 `-` |
| 11 | 122 | 内网信息显示成员数与我的物理地址 | `right_net` 文本合集同时含 `成员 6 人` 与 `我的物理地址` |
| 12 | 130 | 投票页显示申请人 | `switch_nav("vote")` 后 `vote_inner` 文本含 `赵六` |
| 13 | 131 | 投票页显示物理地址 | 文本含 `F1-E2-D3-C4-B5-A6` |
| 14 | 132 | 投票页显示票数门槛（6 人全票） | 文本含 `需 6 票` |
| 15 | 133 | 投票页显示规则说明 | 文本含 `全票通过` |
| 16 | 134 | 投票页有「同意/拒绝/弃权」三个按钮 | 三个按钮都能被 `find_button` 找到 |
| 17 | 141 | 悬浮面板出现 | `_show_vote_popup()` 后 `_vote_popup is not None and _vote_popup.winfo_exists()` |
| 18 | 143 | 面板里有申请卡片 | `_vote_popup_body` 文本同时含 `赵六` 与 `需 6 票` |
| 19 | 146 | 鼠标移开后面板消失 | `_hide_vote_popup()` 后 `app._vote_popup is None` |
| 20 | 153 | 投出同意票 | `find_button(vote_inner,"同意").invoke()` 后 `net_my_votes["ui-req-1"]["approve"] is True` |
| 21 | 154 | 投完小红点消失 | `not app.vote_dot.winfo_ismapped()` |
| 22 | 155 | 投票后卡片显示「我已投：同意」 | `vote_inner` 文本含 `我已投：同意` |
| 23 | 157 | 5 分钟内可改票（页面给出改票按钮） | `find_button(vote_inner,"改拒绝")` 非 None |
| 24 | 160 | 投票页仍展示该进行中的投票 | `vote_inner` 文本仍含 `赵六` |
| 25 | 167 | 历史里有这条投票 | `switch_nav("more")` 后 `hist_tree` 存在某行第 2 列含 `赵六` |
| 26 | 168 | 历史里显示我的投票为同意 | 某行第 4 列含 `同意` |
| 27 | 169 | 历史里标注可改票 | 某行第 4 列含 `可改` |

收尾：轮询 `root.update()` 共 0.5 秒，然后 `app._on_close()`；统计失败并 `sys.exit`。

---

### 5.4 `test_gui_smoke.py` —— 更新机制 + GUI 冒烟，48 条

**环境变量（`import neiwang` 前）**：`NEIWANG_DATA_DIR=<脚本目录>/_testdata_gui`、
`NEIWANG_UDP_PORT=45685`、`NEIWANG_TCP_PORT=45640`、`NEIWANG_SCAN_WINDOW=1`、
`NEIWANG_NET_ANNOUNCE=2`、`PYTHONDONTWRITEBYTECODE=1`；
运行中还会动态改 `NEIWANG_UPDATE_ROOT` 与 `NEIWANG_DATA_DIR`（见下表）。

**临时目录**：`<脚本目录>/_testdata_gui`，先删后建。子目录：
`up1`/`up2`/`up_old`/`up3`/`up4`/`up5`（各是一个 `PeerCore` 的 `data_root`）、
`upd_zip`（ZIP 更新的 `NEIWANG_UPDATE_ROOT`，含 `customer_god/`、`update_clone/`）、
`upd_logic`（取包方式判定的 root）、`upd_git`（真机 Git 的 root）、
`fake_repo`（本地 git 仓库源）、`src_data`（模拟用户 data，含 `config.json`、`downloads/作业.txt`）、
`evil`（危险 ZIP 解压目标）、`cli_data`（`--check` 子进程的 `NEIWANG_DATA_DIR`）。

**内置 HTTP 服务器**：`socketserver.TCPServer(("127.0.0.1", 0), Handler)`（端口 0 = 随机），
后台线程 `serve_forever`。`Handler` 类属性：
- `version`：默认响应体（纯文本）；
- `body`：若设置则**原样返回**（用于带更新内容的清单）；
- `zip_bytes`：`/pkg.zip` 路径返回该字节流（带 `Content-Type: application/zip` 与 `Content-Length`）；
- `do_GET`：路径以 `/pkg.zip` 开头且 `zip_bytes` 非空 → 返回 ZIP；否则返回
  `body if body is not None else version`（UTF-8，`text/plain`，带 `Content-Length`）；
- `log_message` 被覆盖为空操作。
`MANIFEST_URL = http://127.0.0.1:{PORT}/version.txt`。

`build_zip(files: dict) -> bytes`：用 `ZIP_DEFLATED` 把 `{名字: 内容}` 打成内存 ZIP。
`make_cfg(port=None)`：`{"uid":"GUI-TEST-1","name":"GUI测试员","tcp_port":45640,
"udp_port":45685,"groups":[],"update_url": MANIFEST_URL 或 http://127.0.0.1:{port}/version.txt}`。

| # | 行 | 断言名 | 断言内容 |
| --- | --- | --- | --- |
| 1 | 116 | 远程版本更新 -> 提醒 | `version="2.0.0"`，`core.check_update()` 后事件 `("update",{found:真, remote:"2.0.0"})` |
| 2 | 123 | 版本号相同 -> 静默 | `version=M.APP_VERSION` → `{ok:真, found:假}` |
| 3 | 131 | 远程版本更旧 -> 静默 | `version="0.1"` → `{ok:真, found:假}` |
| 4 | 137 | 未联网（连接被拒绝）-> 静默无反馈 | `update_url` 指向 `127.0.0.1:45699`（无监听）→ `{ok:假}` |
| 5 | 145 | 纯文本清单：版本号解析 | `body="2.0.1\n- 修复了 xxx\n- 新增了 yyy"` → `remote=="2.0.1"` |
| 6 | 146 | 纯文本清单：更新内容解析 | `notes` 同时含 `修复了 xxx` 与 `新增了 yyy` |
| 7 | 154 | JSON 清单：版本号解析 | `body='{"version": "2.0.2", "notes": ["修复 A", "新增 B"]}'` → `remote=="2.0.2"` |
| 8 | 155 | JSON 清单：更新内容解析 | `notes` 同时含 `修复 A` 与 `新增 B` |
| 9 | 159 | 本地 update.wenyi 存在 | `M.local_manifest_path().is_file()` |
| 10 | 161 | 本地清单版本号 = 程序版本号 | `load_local_manifest()["version"] == M.APP_VERSION`（非空） |
| 11 | 162 | 本地清单含更新内容 | `bool(lm["notes"])` |
| 12 | 165 | 版本比较：1.1.0 > 1.0 | `is_newer_version("1.1.0","1.0")` |
| 13 | 166 | 版本比较：1.0 与 1.0.0 视为相同 | 双向 `is_newer_version` 都为假 |
| 14 | 168 | 版本比较：更旧不算更新 | `not is_newer_version("0.9","1.0")` |
| 15 | 184 | 程序/文档里已无旧品牌词 | 在 `neiwang.py, neiwang_update.py, test_protocol.py, test_gui_smoke.py, 启动.bat, 更新.bat, update.wenyi, README.md, .gitignore` 九个文件的小写文本里都不含 `"mc"+"tier"` 与 `"mc"+"tire"`；文件缺失也记为失败 |
| 16 | 185 | 主程序已无更新执行代码 | `not hasattr(M.PeerCore,"clone_update") and not hasattr(M,"restore_customer_data")` |
| 17 | 187 | 独立更新程序存在且可被主程序定位 | `U.__file__` 真、`M.updater_path().is_file()`、其 `name=="neiwang_update.py"` |
| 18 | 206 | `--update` -> 直接更新模式 | 把 `U.UpdaterApp` 换成 `_StubApp` 后 `U.main(["--update"])`，`last=="update"` |
| 19 | 209 | `--auto` -> 静默模式 | `U.main(["--auto"])` → `"auto"` |
| 20 | 211 | 无参数 -> 手动模式 | `U.main([])` → `"manual"` |
| 21 | 216 | 更新程序版本比较函数一致 | `is_newer_version("1.2","1.1")` 真、`("1.1","1.1")` 假、`("1.0","1.1")` 假 |
| 22 | 219 | 更新程序使用独立数据目录 | `U.get_data_dir() == TESTDATA` |
| 23 | 224 | 更新程序：发现新版本 -> update | `version="9.9.9"` → `check_for_update({"update_url":MANIFEST_URL})["status"]=="update"` 且 `remote=="9.9.9"` |
| 24 | 227 | 更新程序：版本相同 -> current | `version=U.local_version()` → `status=="current"` |
| 25 | 230 | 更新程序：连不上 -> offline | `update_url="http://127.0.0.1:1/none"` → `status=="offline"` |
| 26 | 232 | 更新程序：没配地址 -> offline | `update_url=""` → `status=="offline"` |
| 27 | 253 | ZIP 更新：整体成功 | `perform_update({"update_repo":"", "update_package": .../pkg.zip})` → `ok` 且 `method=="zip"` |
| 28 | 255 | ZIP 更新：会话内容已备份到 customer_god/ | `upd_root/customer_god/config.json` 与 `.../downloads/作业.txt` 均存在 |
| 29 | 258 | ZIP 更新：新版本代码已就位 | `upd_root/update_clone/neiwang.py` 与 `.../neiwang_update.py` 均存在 |
| 30 | 261 | ZIP 更新：会话内容已回填到新版本 data/ | `update_clone/data/config.json` 存在且 `data/downloads/作业.txt` 内容 == `重要会话内容` |
| 31 | 265 | ZIP 更新：进度日志有输出 | 日志序列中既含 `备份` 又含 `回填` 的字符串 |
| 32 | 290 | 没装 Git 时自动改用 ZIP 更新包 | `have_git=lambda:False`，同时配 `update_repo` 与 `update_package` → `ok` 且 `method=="zip"` |
| 33 | 299 | 有 Git 时优先用 Git 方式 | `have_git=lambda:True` + 假 `run_git_clone`（成功并写出 `neiwang.py`）→ `ok` 且 `method=="git"` |
| 34 | 308 | Git 失败时自动回退到 ZIP | 假 `run_git_clone` 返回失败 + 配了 ZIP → `ok` 且 `method=="zip"` |
| 35 | 334 | Git 更新（真机）：整体成功 | *仅当本机 `have_git()` 且 `perform_update` 成功*：`method=="git"` |
| 36 | 336 | Git 更新（真机）：新版本 + 会话内容都在 | *同一分支*：`upd_git/update_clone/neiwang.py` 存在，且 `data/downloads/作业.txt` 内容正确 |
| 37 | 348 | 既没 Git 仓库也没 ZIP 包 -> 明确报错 | `perform_update({"update_repo":"","update_package":""})` → `ok is False` 且 reason 含 `update_repo` 或 `update_package` |
| 38 | 359/361 | 危险 ZIP（含 ../）被拒绝 | `safe_extract` 解压含成员 `内部网-main/../../evil.txt` 的 ZIP 时必须抛异常（互斥，2 处调用点） |
| 39 | 384 | `--check`：有新版本 -> UPDATE_AVAILABLE | 子进程 `[sys.executable,"-B",UPDATER,"--check"]`（cwd=ROOT，env 覆盖 `NEIWANG_DATA_DIR=cli_data`、`PYTHONDONTWRITEBYTECODE=1`）stdout+stderr 含 `UPDATE_AVAILABLE 9.9.9` |
| 40 | 387 | `--check`：版本相同 -> UP_TO_DATE | 输出含 `UP_TO_DATE` |
| 41 | 389 | `--check`：连不上 -> OFFLINE | 输出含 `OFFLINE` |
| 42 | 391 | `--check 命令行可用` | 上面三条外包 try；子进程异常时以 `repr(e)` 记为失败（互斥分支） |
| 43 | 412 | GUI 启动后自动建立内网 | 20s 内轮询 `app.core.in_net()` |
| 44 | 420 | 内网页面渲染正常 | `isinstance(app.core.net_view()["members"], list)` |
| 45 | 429 | 更新提醒选「取消」后仍可再次提醒 | 把 `M.messagebox.askyesnocancel` 换成返回 `None` 的桩；置 `app._update_shown=False` 后调用 `app._on_update_result({ok:真, found:真, remote:"9.9.9", local:M.APP_VERSION, notes:"- 测试", page:""})`，断言 `app._update_shown is False` |
| 46 | 435 | GUI 启动并退出正常 | 建群 `冒烟测试群` + `select_gid` + `sleep(0.8)` 后 `len(app.core.my_groups()) == 1`；`root.after(600, root.destroy)` 走一遍 `mainloop()` |
| 47 | 439 | GUI 启动并退出正常（异常路径） | GUI 段外层 `except` 时以 `repr(e)` 记为失败（互斥分支） |

（上表 47 个逻辑条目对应 48 个 `ok(` 调用点：`--check 命令行可用`、`危险 ZIP 被拒绝`、
`GUI 启动并退出正常` 三处各含成功/异常互斥的两处调用点，真机 Git 的两条在同一分支内。
第 48–50 行是 `ok()` 断言收集器的定义本身，不产生断言。）

补充要点：
- `--check` 的三条测试会**写 `cli_data/config.json`**（内容只有 `{"update_url": ...}`），
  并以 `subprocess.run(..., capture_output=True, text=True, timeout=60)` 启动更新程序。
- 真机 Git 段：在 `fake_repo` 里 `git init -q` → `git add -A` →
  `git -c user.email=t@t -c user.name=t commit -qm init`（全部 `stdout/stderr=DEVNULL`），
  再 `perform_update({"update_repo": str(fake_repo), "update_package": ""})`（本地路径 clone）。
  `git` 不可用或 clone 失败时打印 `SKIP Git 更新（...）` 而**不**产生失败断言。
- GUI 冒烟前会写 `TESTDATA/config.json`：`{"uid":"GUI-TEST-1","name":"GUI测试员",
  "update_url":"http://127.0.0.1:1/none","update_page":""}`。
- 收尾：`srv.shutdown()`；统计失败并 `sys.exit(1 if failed else 0)`。

---

## 6. `update.wenyi` 当前内容与版本号

文件：`<工作区>/update.wenyi`（11 行，1295 字节，UTF-8，无 BOM）。

- **首行版本号 = `1.1`**（注意：不是 `1.1.0`；主程序常量里的 `1.1.0` 只是读文件失败时的兜底值，
  运行时 `APP_VERSION` 会被覆盖为 `1.1`）。
- 第 2–11 行是 10 条更新内容（纯文本格式，每行以 `- ` 开头）。

逐字内容：

```
1.1
- 新增「内网准入」：局域网里的电脑先组成一个内网，登记每台电脑的物理地址（MAC）；没被接纳的电脑程序是空的
- 启动自动组网：已在内网则直接广播；否则扫描附近内网——扫描到就申请加入，没扫描到就自行建立一个
- 新增「入网投票」：内网 ≤5 人免投票自动通过；6–7 人需全票通过；≥8 人需超过 1/3 同意（1/3 规则不得少于 8 人）
- 新增左侧「投票」入口：待投票时出现小红点，鼠标靠近图标才弹出面板，可同意/拒绝/弃权
- 投票规则：一次投票有效期 1 天（超时视为结束），投出后 5 分钟内可以改票，投完小红点消失
- 新增左侧「内网」页：成员登记表（名字/物理地址/内网 IP/加入时间/角色）、附近内网列表、退出与重命名内网
- 新增左侧「更多」页：投票历史（5 分钟内可改票）、附近内网、常用工具
- 群组、聊天与文件分发均要求先入网：内网外机器发来的消息一律忽略
- 修复：同一台电脑上跑多个实例时，内网扫描应答改用广播（原先单播可能被投递到错误的套接字）
- 私聊、保密转存、改名投票、自动下载与更新检查等既有功能保持不变
```

解析结果（按 §2.1 规则）：
- JSON 解析失败（首行 `1.1` 不是合法 JSON）→ 走纯文本分支；
- `version == "1.1"`；
- `notes` == 上述第 2–11 行用 `\n` 连接并 strip 后的文本。

版本比较推论：远端若要触发更新，其清单版本的数字段必须**严格大于** `(1,1)`——
例如 `1.1.1`、`1.2`、`2` 都触发；`1.1.0`、`1.1`、`1.0`、`v1.1` 都不触发
（`1.1.0 → (1,1,0)` 补 0 后与 `(1,1)` 补 0 后相等）。

---

## 7. 移植验收清单（C++ 侧可直接照抄的判据）

1. `version_key` / `is_newer_version` / `parse_update_manifest` 三个纯函数用同一批输入
   产出与 §2 完全一致的结果（尤其是 `"abc"→(0,)`、`"1.1.0"` 与 `"1.1"` 相等、
   数组 notes 用 `\n` 连接、JSON 优先但有 `version` 键才生效）。
2. 四种命令行形态的模式解析、退出码、`--check` 三行输出、`usage()` 文本逐字一致；
   `--auto --update` → auto；未知参数忽略；`--help` 优先。
3. 更新三步的目录/文件名、日志文案、失败 reason 文案逐字一致；
   `update_clone` 开头必删；Git 成功需校验 `neiwang.py`；ZIP 路径 `finally` 必删 `_update_unzip`；
   回填只遍历备份第一层且覆盖 `update_clone/data/`。
4. `safe_extract` 拒绝 `/` 开头、盘符、整段 `..` 三类成员，报错文案逐字。
5. 主程序侧：启动即检查（timeout 8s、读 65536 字节）、静默条件、
   弹窗文案与「是/否/取消」三行为、`_update_shown` / `_manually_check` / `_updating` 状态位、
   `Popen([python, updater, "--update"], cwd=updater.parent)` 逐字。
6. 四个测试的断言点（§5 表格）全部可复现；`_testdata*` 临时目录隔离；环境变量钩子同名同义。
