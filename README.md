我几乎将此项目的底裤翻给你看了（漏洞/功能等请将邮件发送至3814859587@qq.com）

# MCTier 局域网群组（班群助手）

> 一句话介绍：**纯 Python 标准库**写的局域网群聊 + 群文件共享软件，零第三方依赖、复制即可运行，
> 界面仿 Windows 11 微信 4.x 桌面版，专为同班 / 同小组电脑互传作业、通知、聊天设计。

**语言 / Languages / Языки：**
[简体中文](#一简体中文) ·
[繁體中文](#二繁體中文) ·
[Русский](#三русский) ·
[English](#四english)

---

## 一、简体中文

### 1.1 项目简介

MCTier 局域网群组（简称「班群助手」）是一个**纯 Python 标准库**实现的局域网群组软件：

- **零第三方依赖**：只用 `socket / threading / tkinter / hashlib / hmac / json / urllib / subprocess` 等标准库，把文件夹复制过去就能跑；
- **无需服务器**：没有中心服务端，所有电脑平等互联，谁先建群谁就是群主电脑，其他人凭群号加入；
- **界面仿微信 4.x 桌面版**：左侧窄图标栏（聊天 / 日志）+ 中间群组列表（搜索、未读红点、右键菜单）+ 右侧聊天气泡主区（微信绿、圆形头像、群文件、成员列表）；
- **面向班级场景**：作业互传、通知广播、临时聊天，局域网内不需要外网也能用。

### 1.2 运行环境与依赖（重要，请先看这里）

| 项目 | 要求 |
| --- | --- |
| Python | **3.8 或更高**（推荐 3.10+）。Python 官方安装包**默认自带 tkinter**，不需要额外装界面库 |
| 操作系统 | **Windows 10 / 11（推荐，体验最好）**；Windows 7 / 8.1、Linux、macOS 也能运行 |
| 第三方库 | **零依赖**，完全不需要 `pip install` 任何东西 |
| 网络 | 所有电脑必须处于**同一局域网 / 同一网段**（同一个路由器、交换机或校园网），并且该网络允许 **UDP 广播** |
| 端口 | UDP **45455**（广播与设备发现）、TCP **45431**（文件传输、私聊转存；被占用时自动向后试 20 个端口） |
| 防火墙 | 首次运行 Windows 会弹窗，必须点**允许**（至少勾选「专用网络」），否则别人看不到你 |
| 权限 | 普通用户权限即可，**不需要管理员** |
| 磁盘占用 | 程序本体 < 200 KB；聊天记录、文件缓存都在 `data/` 目录，按实际收发的文件大小增长 |
| Git（可选） | **只有使用「克隆更新」功能时才需要**安装 Git，平时完全用不到 |

**各系统补充说明：**

- **Windows（推荐）**：支持右下角**系统气泡通知**（调用 `Shell_NotifyIcon`）、Win11 原生圆角窗口；
  双击 `启动.bat` 即可运行（脚本优先用 `pythonw`，不会残留黑色控制台窗口）。
- **Linux**：需要额外安装 tkinter（Debian/Ubuntu：`sudo apt install python3-tk`；Fedora：`sudo dnf install python3-tkinter`）；
  系统通知会退化为程序内弹窗；打开文件用 `xdg-open`。
- **macOS**：请使用 python.org 官方安装的 Python（自带 tkinter）；通知退化为弹窗；打开文件用 `open`。
- **网络环境**：如果学校 Wi-Fi 开启了「AP 隔离 / 客户端隔离」，广播包会被丢弃，此时互相看不见——
  请改用同一个路由器/交换机的有线或普通 Wi-Fi。

### 1.3 功能一览

| 功能 | 说明 |
| --- | --- |
| 文字群聊 | 群内实时聊天，气泡样式（自己绿色靠右、对方白色靠左），卡片显示预览与未读红点 |
| 双人私聊 | 群里点成员头像（或聊天里点对方头像）即可开私聊窗口，可同时开多个 |
| 离线保密转存 | 对方不在线时，消息**加密**暂存在某位在线同学电脑上；对方一上线自动送达，校验完整后删除转存 |
| 群文件 | 向群组发送文件，组内成员**自动下载**；列表双击用系统默认程序打开 |
| 离线文件回退 | 发送人关机也不怕：在公共日志里自动询问，其他下载过该文件的人自动提供 |
| 改名投票 | 每个人自选名字，**第一次改名自由**，之后需超过 5 名用户投票同意 |
| 公共日志 | 「日志」页：全网公共日志，所有人可见、**只读** |
| 检查更新 | 联网时校验 `update.wenyi`，弹窗显示「新版本 ——（版本号）」与更新内容，可一键**克隆更新** |
| 系统通知 | 文件下载完成弹系统通知：`「qqq」群「AA」的「你好」下载完毕` |

### 1.4 快速开始

1. 每台电脑安装 **Python 3.8+**（Windows 安装时记得勾选 *Add Python to PATH*）。
2. 把整个文件夹复制到每台电脑（U 盘 / 网盘 / 共享文件夹都行，不需要放到同一路径）。
3. Windows 双击 **`启动.bat`**；其他系统命令行运行 `python mctier_lan.py`。
4. **创建群**：点「＋ 创建群组」输入群名（如：高三（2）班），程序自动生成 **6 位群号**。
5. **加群**：其他同学点「加入群组」→ 自动扫描局域网里发现的群 → 选中加入；也可以手动输入群号。
6. **改名**：点左下角头像（或「设置」）→ 输入新名字。**第一次改名直接生效**；之后的修改会发起投票。
7. **聊天**：选中群组 → 打字回车发送。
8. **私聊**：群内切到「成员」页 → 点某位同学头像（或直接点聊天里对方的头像）→ 弹出私聊窗口。
9. **发文件**：点「📎 发送文件」→ 选文件 → 全组成员自动下载 → 完成后弹系统通知。
10. **看文件**：切到「群文件」页，双击文件即可用系统默认程序打开。

> 建议：群号告诉大家，由一台电脑创建群组即可，所有加入者共享同一个群。

### 1.5 改名投票规则

- **第一次改名**：在「设置」里直接保存，立即生效，不需要任何人同意。
- **第二次及以后**：点「保存名字」会向局域网广播投票请求，其他在线用户弹出
  「某某申请把名字从 A 改为 B，是否同意？」→ 点「同意 / 拒绝」。
- **生效条件**：收到**超过 5 名用户**的同意票（默认值，可在 `data/config.json` 的
  `rename_vote_need` 修改）；**120 秒**内票数不足则失败，可以重新发起。
- **防刷票**：同一台电脑对同一次投票只计一票；每次投票都有唯一编号，互不干扰。
- 申请与通过结果都会写入公共日志，全员可见。

### 1.6 双人私聊与离线保密转存

**私聊入口**：群「成员」页点成员头像，或聊天区点对方头像 / 名字。

**在线时**：消息定向发送，只有收件人收到，其他人不显示、不记日志。

**对方离线时（保密转存）**，过程完全自动：

1. 你的程序把消息用**随机密钥加密**（每条消息一把新密钥），转存给某位在线的同学 C；
2. **C 全程无感**：没有弹窗、没有日志、界面上看不到，磁盘里只有密文；
3. 对方 B 一上线（心跳广播），C 的程序自动把密文包直连送达给 B；
4. B 解密并做 **HMAC 完整性校验**，完整才回执；C 收到回执后**删除**转存内容；
   校验失败则保留重试（最多 3 次）；
5. B 的界面像普通私聊一样显示，并带「离线转存送达」标记。

> 说明：转存使用纯标准库流加密 + HMAC 做「机器级」保密与完整性校验（转存方用户无法在应用里查看）。
> 如需抵抗「转存方直接翻磁盘」的更强保密，可升级为非对称加密。

### 1.7 群文件分发策略

1. **发送人在线** → 接收者直接向发送人电脑**点对点下载**（SHA-256 校验，损坏自动重试）。
2. **发送人离线** → 在「日志」里**公共询问**（全网可见、人人可看不可操作）：
   其他下载过该文件的电脑自动返回自己的局域网 IP（没下载过则无应答），
   请求方自动向**第一个返回的 IP** 请求文件，全程无需两端用户操作。
3. 加入群组时会自动同步该群文件列表，错过的文件会自动补下载。

### 1.8 版本与更新机制

**清单文件 `update.wenyi`**（放在程序目录，同时也放一份到 GitHub / 你的网站）：

```
1.1.0
- 新增双人私聊
- 新增离线保密转存
```

- **第一行 = 版本号**，其余各行 = 本次更新内容（也可以写成 JSON：`{"version": "1.1.0", "notes": ["..."]}`）。
- 程序启动时（联网状态下）自动请求 `update_url`（默认读取 GitHub 上的 `update.wenyi`）校验；
  也可以在「设置 → 检查更新」手动触发。
- 程序**自己的版本号也取自本机 `update.wenyi` 的第一行**——发布新版本时只改这一个文件即可。
- 版本号相同 → **无任何反馈**；请求失败（没联网）→ **无任何反馈**；版本号不同 → 弹窗：

```
新版本 —— v1.1.1

更新内容：
- 修复了 xxx
- 新增了 yyy

（当前版本 v1.1.0）

「是」立即克隆更新：程序自动把新版本克隆下来，
你的所有会话内容会保存到 customer_god 文件夹并接着用；
「否」打开更新页面手动下载；「取消」稍后再说。
```

**克隆更新（更新方式：克隆）** 分三步，全部自动完成：

1. 把当前 `data/` 目录（**用户所有会话内容**：群组、聊天、文件索引、转存记录）完整备份到
   程序目录下的新文件夹 **`customer_god/`**；
2. 用 `git clone`（浅克隆）把最新版本克隆到程序目录的 **`update_clone/`**；
3. 把 `customer_god/` 里的会话内容**回填**到新版本的 `data/` 目录。

完成后弹窗提示新版本路径，关闭旧程序、到新目录双击 `启动.bat` 即可，聊天和文件都不会丢。
失败时会明确提示原因（例如没装 Git、仓库地址没配置、网络不通）。

**相关配置（`data/config.json`）**：

| 字段 | 说明 |
| --- | --- |
| `update_url` | 更新清单地址（已默认指向本仓库 raw）：`https://raw.githubusercontent.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-/main/update.wenyi` |
| `update_page` | 「否」按钮打开的更新页（如你的 GitHub Releases 页面） |
| `update_repo` | 克隆更新用的仓库地址：`https://github.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-.git` |

> 提示：这两个地址已默认指向上面的仓库。若你的默认分支是 `master` 而不是 `main`，请把代码（或 `config.json`）里的 `main` 改成 `master`。

### 1.9 数据目录

```
data/
├─ config.json          # 配置：名字、群组、端口、更新地址、改名投票设置
├─ index.json           # 文件索引（指纹 -> 文件名 / 发送人 / 群组）
├─ cache/<指纹>          # 文件内容缓存（也用于给别人提供下载）
├─ downloads/<群名>_<群号>/  # 自动下载的文件（按群分文件夹）
├─ secret_relay.json    # 替别人暂存的离线私聊（密文 + 签名），送达后自动删除
└─ secret_received.json # 已收到的转存编号（去重，避免重复显示）

customer_god/           # 克隆更新时备份的「用户所有会话内容」
update_clone/           # 克隆更新下来的新版本（含 data/）
update.wenyi            # 版本清单：版本号 + 更新内容
```

### 1.10 常见问题

- **两台电脑互相看不见？** 确认在同一局域网同一网段；允许 Python 通过防火墙；确认路由器没开 AP 隔离。
- **端口被占用？** 程序会自动尝试 45431 之后的端口；UDP 45455 只用于广播发现。
- **发送人关机后文件还能下吗？** 能——只要有其他同学下载过该文件并在线，就会自动从他那里取。
- **退出群组会删文件吗？** 不会，已下载文件保留在本机。
- **改名改不了了？** 说明你已经改过一次，需要超过 5 位同学投票同意；人数不够时可以在 `config.json` 调低 `rename_vote_need`。
- **私聊对方离线会丢消息吗？** 不会，会自动加密转存到在线同学处，对方上线后自动送达。
- **克隆更新失败？** 检查是否装了 Git（`git --version`）、是否配置了 `update_repo`、网络能否访问 GitHub。
- **怎么干净退出？** 点窗口关闭按钮即可，所有状态自动保存。

### 1.11 开发与测试

```bash
python test_protocol.py    # 无头协议测试：群聊、私聊、保密转存、文件下载、离线回退、改名投票
python test_gui_smoke.py   # GUI 冒烟 + 更新检查 + 清单解析 + 克隆更新（备份/回填）测试
```

- 测试会使用 `_testdata/`、`_testdata_gui/` 作为数据目录，不影响你的 `data/`；
- 可用环境变量加速测试：`MCTIER_PRES_INTERVAL`、`MCTIER_PRES_TTL`、`MCTIER_VOTE_NEED`、`MCTIER_VOTE_WAIT`、`MCTIER_DATA_DIR`、`MCTIER_UPDATE_ROOT` 等。

### 1.12 联系方式

漏洞、建议、功能需求都欢迎发邮件到 **3814859587@qq.com**。

---

## 二、繁體中文

### 2.1 專案簡介

MCTier 區域網路群組（簡稱「班群助手」）是一套**純 Python 標準函式庫**實作的區域網路群組軟體：

- **零第三方相依**：只使用 `socket / threading / tkinter / hashlib / hmac / json / urllib / subprocess` 等標準函式庫，整個資料夾複製過去就能執行；
- **不需要伺服器**：沒有中央伺服器，所有電腦平等互連，先建立群組的人就是群主電腦，其他人用群號加入；
- **介面仿微信 4.x 桌面版**：左側窄圖示列（聊天 / 日誌）+ 中間群組清單（搜尋、未讀紅點、右鍵選單）+ 右側聊天泡泡主區（微信綠、圓形頭像、群檔案、成員清單）；
- **針對班級情境**：作業互傳、通知廣播、臨時聊天，區域網路內不需要外網也能使用。

### 2.2 執行環境與相依（重要，請先看這裡）

| 項目 | 需求 |
| --- | --- |
| Python | **3.8 或更高**（建議 3.10+）。Python 官方安裝程式**預設自帶 tkinter**，不需額外安裝介面函式庫 |
| 作業系統 | **Windows 10 / 11（建議，體驗最佳）**；Windows 7 / 8.1、Linux、macOS 也能執行 |
| 第三方函式庫 | **零相依**，完全不需要 `pip install` 任何東西 |
| 網路 | 所有電腦必須位於**同一個區域網路 / 同一網段**（同一台路由器、交換器或校園網路），且該網路允許 **UDP 廣播** |
| 連接埠 | UDP **45455**（廣播與裝置探索）、TCP **45431**（檔案傳輸、私聊轉存；被占用時自動往後嘗試 20 個連接埠） |
| 防火牆 | 首次執行 Windows 會跳窗，必須點**允許**（至少勾選「私人網路」），否則別人看不到你 |
| 權限 | 一般使用者權限即可，**不需要系統管理員** |
| 磁碟用量 | 程式本體 < 200 KB；聊天紀錄、檔案快取都在 `data/` 目錄，依實際收發的檔案大小成長 |
| Git（選用） | **只有使用「複製更新」功能時才需要**安裝 Git，平時完全用不到 |

**各系統補充說明：**

- **Windows（建議）**：支援右下角**系統泡泡通知**（呼叫 `Shell_NotifyIcon`）、Win11 原生圓角視窗；
  雙擊 `啟動.bat` 即可執行（指令碼優先使用 `pythonw`，不會殘留黑色主控台視窗）。
- **Linux**：需要額外安裝 tkinter（Debian/Ubuntu：`sudo apt install python3-tk`；Fedora：`sudo dnf install python3-tkinter`）；
  系統通知會退化為程式內彈窗；開啟檔案使用 `xdg-open`。
- **macOS**：請使用 python.org 官方安裝的 Python（自帶 tkinter）；通知退化為彈窗；開啟檔案使用 `open`。
- **網路環境**：若學校 Wi-Fi 開啟「AP 隔離 / 用戶端隔離」，廣播封包會被丟棄，此時彼此看不見——
  請改用同一個路由器/交換器的有線或一般 Wi-Fi。

### 2.3 功能一覽

| 功能 | 說明 |
| --- | --- |
| 文字群聊 | 群組內即時聊天，泡泡樣式（自己綠色靠右、對方白色靠左），卡片顯示預覽與未讀紅點 |
| 雙人私聊 | 群組內點成員頭像（或聊天中點對方頭像）即可開啟私聊視窗，可同時開多個 |
| 離線保密轉存 | 對方不在線時，訊息**加密**暫存在某位在線同學的電腦上；對方一上線自動送達，校驗完整後刪除轉存 |
| 群組檔案 | 向群組傳送檔案，群組成員**自動下載**；清單雙擊以系統預設程式開啟 |
| 離線檔案回退 | 傳送者關機也不怕：在公共日誌中自動詢問，其他下載過該檔案的人自動提供 |
| 改名投票 | 每人自選名字，**第一次改名自由**，之後需超過 5 名使用者投票同意 |
| 公共日誌 | 「日誌」頁：全網公共日誌，所有人可見、**唯讀** |
| 檢查更新 | 連網時校驗 `update.wenyi`，彈窗顯示「新版本 ——（版本號）」與更新內容，可一鍵**複製更新** |
| 系統通知 | 檔案下載完成彈系統通知：`「qqq」群「AA」的「你好」下載完畢` |

### 2.4 快速開始

1. 每台電腦安裝 **Python 3.8+**（Windows 安裝時記得勾選 *Add Python to PATH*）。
2. 把整個資料夾複製到每台電腦（USB 隨身碟 / 雲端硬碟 / 共用資料夾都可以，不需要放在相同路徑）。
3. Windows 雙擊 **`啟動.bat`**；其他系統於命令列執行 `python mctier_lan.py`。
4. **建立群組**：點「＋ 建立群組」輸入群組名稱（例如：高三（2）班），程式自動產生 **6 位群號**。
5. **加入群組**：其他同學點「加入群組」→ 自動掃描區域網路中發現的群組 → 選取加入；也可手動輸入群號。
6. **改名**：點左下角頭像（或「設定」）→ 輸入新名字。**第一次改名直接生效**；之後的修改會發起投票。
7. **聊天**：選取群組 → 打字按 Enter 傳送。
8. **私聊**：群組內切到「成員」頁 → 點某位同學的頭像（或直接點聊天中對方的頭像）→ 彈出私聊視窗。
9. **傳檔案**：點「📎 傳送檔案」→ 選檔案 → 全群組成員自動下載 → 完成後彈系統通知。
10. **看檔案**：切到「群組檔案」頁，雙擊檔案即可用系統預設程式開啟。

> 建議：把群號告訴大家，由一台電腦建立群組即可，所有加入者共用同一個群組。

### 2.5 改名投票規則

- **第一次改名**：在「設定」中直接儲存，立即生效，不需要任何人同意。
- **第二次及之後**：點「儲存名字」會向區域網路廣播投票請求，其他線上使用者會彈出
  「某某申請把名字從 A 改為 B，是否同意？」→ 點「同意 / 拒絕」。
- **生效條件**：收到**超過 5 名使用者**的同意票（預設值，可在 `data/config.json` 的
  `rename_vote_need` 修改）；**120 秒**內票數不足則失敗，可以重新發起。
- **防灌票**：同一台電腦對同一次投票只計一票；每次投票都有唯一編號，互不干擾。
- 申請與通過結果都會寫入公共日誌，全員可見。

### 2.6 雙人私聊與離線保密轉存

**私聊入口**：群組「成員」頁點成員頭像，或聊天區點對方頭像 / 名字。

**對方在線時**：訊息定向傳送，只有收件者收到，其他人不顯示、不記日誌。

**對方離線時（保密轉存）**，過程完全自動：

1. 你的程式把訊息用**隨機金鑰加密**（每則訊息一把新金鑰），轉存給某位在線的同學 C；
2. **C 全程無感**：沒有彈窗、沒有日誌、介面上看不到，磁碟裡只有密文；
3. 對方 B 一上線（心跳廣播），C 的程式自動把密文包直連送達給 B；
4. B 解密並做 **HMAC 完整性校驗**，完整才回執；C 收到回執後**刪除**轉存內容；
   校驗失敗則保留重試（最多 3 次）；
5. B 的介面像一般私聊一樣顯示，並帶「離線轉存送達」標記。

> 說明：轉存使用純標準函式庫串流加密 + HMAC 做「機器級」保密與完整性校驗（轉存方使用者無法在應用程式裡查看）。
> 如需抵抗「轉存方直接翻磁碟」的更強保密，可升級為非對稱加密。

### 2.7 群組檔案分發策略

1. **傳送者在線** → 接收者直接向傳送者電腦**點對點下載**（SHA-256 校驗，損壞自動重試）。
2. **傳送者離線** → 在「日誌」中**公共詢問**（全網可見、人人可看不可操作）：
   其他下載過該檔案的電腦自動回報自己的區域網路 IP（沒下載過則無回應），
   請求方自動向**第一個回應的 IP** 請求檔案，全程無需兩端使用者操作。
3. 加入群組時會自動同步該群組檔案清單，錯過的檔案會自動補下載。

### 2.8 版本與更新機制

**清單檔案 `update.wenyi`**（放在程式目錄，同時也放一份到 GitHub / 你的網站）：

```
1.1.0
- 新增雙人私聊
- 新增離線保密轉存
```

- **第一行 = 版本號**，其餘各行 = 本次更新內容（也可寫成 JSON：`{"version": "1.1.0", "notes": ["..."]}`）。
- 程式啟動時（連網狀態下）自動請求 `update_url`（預設讀取 GitHub 上的 `update.wenyi`）校驗；
  也可在「設定 → 檢查更新」手動觸發。
- 程式**自己的版本號也取自本機 `update.wenyi` 的第一行**——發布新版本時只要改這一個檔案。
- 版本號相同 → **無任何回應**；請求失敗（未連網）→ **無任何回應**；版本號不同 → 彈窗：

```
新版本 —— v1.1.1

更新內容：
- 修復了 xxx
- 新增了 yyy

（目前版本 v1.1.0）

「是」立即複製更新：程式自動把新版本複製下來，
你的所有會話內容會儲存到 customer_god 資料夾並繼續使用；
「否」開啟更新頁面手動下載；「取消」稍後再說。
```

**複製更新（更新方式：複製 / clone）** 分三步，全部自動完成：

1. 把目前 `data/` 目錄（**使用者所有會話內容**：群組、聊天、檔案索引、轉存紀錄）完整備份到
   程式目錄下的新資料夾 **`customer_god/`**；
2. 用 `git clone`（淺層複製）把最新版本複製到程式目錄的 **`update_clone/`**；
3. 把 `customer_god/` 裡的會話內容**回填**到新版本的 `data/` 目錄。

完成後彈窗提示新版本路徑，關閉舊程式、到新目錄雙擊 `啟動.bat` 即可，聊天和檔案都不會遺失。
失敗時會明確提示原因（例如沒安裝 Git、倉庫位址沒設定、網路不通）。

**相關設定（`data/config.json`）**：

| 欄位 | 說明 |
| --- | --- |
| `update_url` | 更新清單位址（已預設指向本儲存庫 raw）：`https://raw.githubusercontent.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-/main/update.wenyi` |
| `update_page` | 「否」按鈕開啟的更新頁（例如你的 GitHub Releases 頁面） |
| `update_repo` | 複製更新用的儲存庫位址：`https://github.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-.git` |

> 提示：這兩個位址已預設指向上面的儲存庫。若你的預設分支是 `master` 而不是 `main`，
> 請把程式碼（或 `config.json`）裡的 `main` 改成 `master`。

### 2.9 資料目錄

```
data/
├─ config.json          # 設定：名字、群組、連接埠、更新位址、改名投票設定
├─ index.json           # 檔案索引（指紋 -> 檔名 / 傳送者 / 群組）
├─ cache/<指紋>          # 檔案內容快取（也用於提供給別人下載）
├─ downloads/<群名>_<群號>/  # 自動下載的檔案（依群組分資料夾）
├─ secret_relay.json    # 替別人暫存的離線私聊（密文 + 簽章），送達後自動刪除
└─ secret_received.json # 已收到的轉存編號（去重，避免重複顯示）

customer_god/           # 複製更新時備份的「使用者所有會話內容」
update_clone/           # 複製更新下來的新版本（含 data/）
update.wenyi            # 版本清單：版本號 + 更新內容
```

### 2.10 常見問題

- **兩台電腦互相看不見？** 確認在同一區域網路同一網段；允許 Python 通過防火牆；確認路由器沒開 AP 隔離。
- **連接埠被占用？** 程式會自動嘗試 45431 之後的連接埠；UDP 45455 只用於廣播探索。
- **傳送者關機後檔案還能下載嗎？** 可以——只要有其他同學下載過該檔案且在線，就會自動從他那裡取得。
- **退出群組會刪檔案嗎？** 不會，已下載檔案保留在本機。
- **改名改不了？** 表示你已經改過一次，需要超過 5 位同學投票同意；人數不足時可在 `config.json` 調低 `rename_vote_need`。
- **私聊對方離線會丟訊息嗎？** 不會，會自動加密轉存到在線同學處，對方上線後自動送達。
- **複製更新失敗？** 檢查是否安裝 Git（`git --version`）、是否設定了 `update_repo`、網路能否存取 GitHub。
- **怎麼乾淨結束？** 點視窗關閉按鈕即可，所有狀態自動儲存。

### 2.11 開發與測試

```bash
python test_protocol.py    # 無頭協定測試：群聊、私聊、保密轉存、檔案下載、離線回退、改名投票
python test_gui_smoke.py   # GUI 冒煙 + 更新檢查 + 清單解析 + 複製更新（備份/回填）測試
```

- 測試會使用 `_testdata/`、`_testdata_gui/` 作為資料目錄，不影響你的 `data/`；
- 可用環境變數加速測試：`MCTIER_PRES_INTERVAL`、`MCTIER_PRES_TTL`、`MCTIER_VOTE_NEED`、`MCTIER_VOTE_WAIT`、`MCTIER_DATA_DIR`、`MCTIER_UPDATE_ROOT` 等。

### 2.12 聯絡方式

漏洞、建議、功能需求都歡迎寄信到 **3814859587@qq.com**。

---

## 三、Русский

### 3.1 О проекте

MCTier LAN Group («помощник для класса») — программа для группового общения и обмена файлами
в локальной сети, написанная **только на стандартной библиотеке Python**.

- **Без сторонних зависимостей**: используются лишь `socket / threading / tkinter / hashlib / hmac / json / urllib / subprocess` — достаточно скопировать папку и запустить.
- **Без сервера**: центрального сервера нет, все компьютеры равноправны; тот, кто создал группу, становится её «хостом», остальные подключаются по номеру группы.
- **Интерфейс в стиле WeChat 4.x для Windows 11**: узкая панель иконок слева (чат / журнал), список групп в центре (поиск, красные индикаторы непрочитанного, контекстное меню), справа — область чата с «пузырями» (зелёный акцент, круглые аватары, файлы группы, список участников).
- **Для школы**: обмен домашними заданиями, объявления, переписка — всё внутри локальной сети, интернет не требуется.

### 3.2 Требования к окружению (важно)

| Пункт | Требование |
| --- | --- |
| Python | **3.8 или новее** (рекомендуется 3.10+). Официальный установщик Python **уже включает tkinter** |
| Операционная система | **Windows 10 / 11 (рекомендуется)**; также Windows 7 / 8.1, Linux, macOS |
| Сторонние библиотеки | **нет**, `pip install` не нужен вообще |
| Сеть | все компьютеры в **одной локальной сети / одном сегменте** (один роутер, коммутатор или школьная сеть), широковещательные **UDP**-пакеты разрешены |
| Порты | UDP **45455** (обнаружение), TCP **45431** (передача файлов, пересылка личных сообщений; при занятости порт автоматически сдвигается, до 20 попыток) |
| Брандмауэр | при первом запуске Windows появится запрос — нажмите **«Разрешить»** (хотя бы для частных сетей), иначе вас не увидят |
| Права | достаточно обычного пользователя, **администратор не нужен** |
| Диск | сама программа < 200 КБ; история и кэш файлов — в папке `data/` |
| Git (необязательно) | нужен **только** для функции «обновление клонированием» |

**Особенности платформ:**

- **Windows (рекомендуется)**: системные всплывающие уведомления (`Shell_NotifyIcon`), скруглённые окна Windows 11;
  запуск — двойной щелчок по `启动.bat` (скрипт использует `pythonw`, консоль не появляется).
- **Linux**: нужен tkinter (`sudo apt install python3-tk` в Debian/Ubuntu, `sudo dnf install python3-tkinter` в Fedora);
  уведомления показываются окном внутри программы; файлы открываются через `xdg-open`.
- **macOS**: используйте Python с python.org (в нём есть tkinter); уведомления — окном; файлы открываются через `open`.
- **Сеть**: если в школьном Wi-Fi включена изоляция клиентов (AP isolation), широковещательные пакеты
  отбрасываются, и компьютеры не видят друг друга — используйте один роутер/коммутатор или обычный Wi-Fi.

### 3.3 Возможности

| Возможность | Описание |
| --- | --- |
| Групповой чат | Обмен сообщениями в реальном времени, «пузыри» (свои — зелёные справа, чужие — белые слева), счётчик непрочитанного |
| Личные сообщения | Нажмите на аватар участника в группе (или на аватар в чате) — откроется отдельное окно личной переписки; окон может быть несколько |
| Секретная пересылка офлайн | Если получатель не в сети, сообщение **шифруется** и временно хранится у другого участника; когда получатель появится, оно доставляется автоматически, проверяется на целостность и удаляется у хранителя |
| Файлы группы | Отправка файлов в группу, участники **скачивают автоматически**; двойной щелчок открывает файл системной программой |
| Резервный источник файлов | Если отправитель выключил компьютер, программа публично спрашивает в журнале — тот, у кого файл уже есть, отдаёт его |
| Голосование за смену имени | Имя выбирается свободно; **первая смена — бесплатно**, дальше нужно больше 5 голосов «за» |
| Общий журнал | Вкладка «Журнал»: общий журнал локальной сети, виден всем, **только для чтения** |
| Проверка обновлений | При наличии сети проверяется `update.wenyi`, окно показывает «Новая версия — (номер)» и список изменений, можно обновиться клонированием |
| Системные уведомления | По завершении загрузки файла: `«qqq»群「AA」的「你好」下载完毕` |

### 3.4 Быстрый старт

1. Установите **Python 3.8+** на каждый компьютер (в Windows отметьте *Add Python to PATH*).
2. Скопируйте всю папку на каждый компьютер (USB-флешка, облако, общая папка — путь может быть любым).
3. В Windows запустите **`启动.bat`**; в других системах — `python mctier_lan.py` в терминале.
4. **Создать группу**: «＋ 创建群组» → введите название (например, «10-А класс») → программа создаст **6-значный номер группы**.
5. **Войти в группу**: «加入群组» → автоматический поиск групп в сети → выберите и войдите; либо введите номер вручную.
6. **Сменить имя**: аватар в левом нижнем углу (или «设置») → введите новое имя. **Первая смена применяется сразу**, дальше — голосование.
7. **Чат**: выберите группу → введите текст и нажмите Enter.
8. **Личные сообщения**: вкладка «成员» → нажмите на аватар участника (или на аватар в чате).
9. **Файлы**: «📎 发送文件» → выберите файлы → участники скачают автоматически → придёт системное уведомление.
10. **Просмотр файлов**: вкладка «群文件» → двойной щелчок открывает файл.

> Совет: сообщите всем номер группы; группу достаточно создать на одном компьютере — все войдут в неё же.

### 3.5 Правила голосования за смену имени

- **Первая смена имени** — сохраняется сразу, без чьего-либо согласия.
- **Вторая и последующие** — при нажатии «保存名字» в сеть отправляется запрос на голосование;
  у остальных участников появляется окно «Такой-то хочет сменить имя с A на B — согласны?» с кнопками «同意 / 拒绝».
- **Условие принятия**: **больше 5 голосов «за»** (значение по умолчанию, меняется в `data/config.json`, поле `rename_vote_need`).
  Если за **120 секунд** голосов не хватило — попытка отклоняется, можно начать заново.
- **Защита от накрутки**: один компьютер — один голос за одно голосование; у каждого голосования уникальный номер.
- Заявка и результат попадают в общий журнал и видны всем.

### 3.6 Личные сообщения и секретная пересылка офлайн

**Как открыть**: аватар участника на вкладке «成员» либо аватар/имя в области чата.

**Если получатель в сети**: сообщение адресное — его видит только получатель, остальные ничего не видят, в журнал оно не попадает.

**Если получатель не в сети (секретная пересылка)** — всё происходит автоматически:

1. ваша программа **шифрует** сообщение случайным ключом (для каждого сообщения — новый ключ) и передаёт его участнику C, который сейчас в сети;
2. **C ничего не замечает**: нет окон, нет записей в журнале, в интерфейсе ничего не видно, на диске — только шифротекст;
3. как только получатель B появляется в сети (широковещательный «пульс»), программа C сама отправляет ему зашифрованный пакет напрямую;
4. B расшифровывает пакет и проверяет **HMAC-подпись целостности**; только при успехе отправляется подтверждение,
   после чего C **удаляет** у себя сохранённое; при ошибке проверки данные остаются и повторяется попытка (до 3 раз);
5. у B сообщение выглядит как обычное личное сообщение с пометкой «离线转存送达» (доставлено из офлайн-хранилища).

> Примечание: используется потоковый шифр из стандартной библиотеки + HMAC — это «машинный» уровень секретности
> (пользователь компьютера-хранителя не может посмотреть сообщение в приложении). Для защиты от чтения файлов
> на диске хранителя потребовалось бы асимметричное шифрование.

### 3.7 Как распространяются файлы

1. **Отправитель в сети** → получатель качает файл **напрямую** у отправителя (проверка SHA-256, при повреждении — повтор).
2. **Отправитель не в сети** → в «Журнале» публичный запрос (виден всем, но выполнить его может только программа):
   компьютеры, у которых файл уже есть, автоматически сообщают свой IP; запрашивающий берёт файл у **первого ответившего**.
3. При входе в группу список файлов синхронизируется автоматически — пропущенные файлы докачиваются.

### 3.8 Версии и обновление

**Файл-манифест `update.wenyi`** (лежит рядом с программой; копия публикуется на GitHub / вашем сайте):

```
1.1.0
- Добавлены личные сообщения
- Добавлена секретная пересылка офлайн
```

- **Первая строка — номер версии**, остальные строки — список изменений (можно и в формате JSON: `{"version": "1.1.0", "notes": ["..."]}`).
- При запуске (если есть сеть) программа запрашивает `update_url` (по умолчанию читает `update.wenyi` с GitHub);
  проверить можно и вручную: «设置 → 检查更新».
- **Версия самой программы тоже берётся из первой строки локального `update.wenyi`** — при выпуске новой версии правится один файл.
- Версия совпадает → **никаких сообщений**; нет сети → **никаких сообщений**; версия отличается → окно:

```
新版本 —— v1.1.1

更新内容：
- 修复了 xxx
- 新增了 yyy

（当前版本 v1.1.0）

«是» — обновить клонированием: программа сама скачает новую версию,
все ваши данные переписки сохранятся в папке customer_god и будут перенесены;
«否» — открыть страницу загрузки; «取消» — позже.
```

**Обновление клонированием** выполняется в три шага, полностью автоматически:

1. текущая папка `data/` (**все данные пользователя**: группы, переписка, индекс файлов, хранилище)
   целиком копируется в новую папку **`customer_god/`** рядом с программой;
2. последняя версия клонируется командой `git clone` (неглубоко) в папку **`update_clone/`**;
3. содержимое `customer_god/` **возвращается** в папку `data/` новой версии.

После этого появляется окно с путём новой версии: закройте старую программу, зайдите в новую папку
и запустите `启动.bat` — переписка и файлы сохранятся. При ошибке будет указана причина
(нет Git, не задан адрес репозитория, нет доступа к сети).

**Настройки (`data/config.json`)**:

| Поле | Назначение |
| --- | --- |
| `update_url` | Адрес манифеста (уже указывает на raw этого репозитория): `https://raw.githubusercontent.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-/main/update.wenyi` |
| `update_page` | Страница загрузки для кнопки «否» (например, страница GitHub Releases) |
| `update_repo` | Адрес репозитория для клонирования: `https://github.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-.git` |

> Примечание: эти адреса уже указывают на репозиторий выше. Если ваша ветка по умолчанию — `master`,
> а не `main`, замените `main` в коде (или в `config.json`) на `master`.

### 3.9 Каталоги данных

```
data/
├─ config.json          # настройки: имя, группы, порты, адрес обновлений, голосование за имя
├─ index.json           # индекс файлов (хэш -> имя / отправитель / группа)
├─ cache/<хэш>           # кэш содержимого файлов (используется и для раздачи другим)
├─ downloads/<группа>_<номер>/  # скачанные файлы (по папкам групп)
├─ secret_relay.json    # чужие офлайн-сообщения (шифротекст + подпись), удаляются после доставки
└─ secret_received.json # номера уже полученных пересылок (защита от повторов)

customer_god/           # резервная копия «всех данных пользователя» при обновлении
update_clone/           # новая версия, полученная клонированием (вместе с data/)
update.wenyi            # манифест: номер версии + список изменений
```

### 3.10 Частые вопросы

- **Компьютеры не видят друг друга?** Проверьте, что они в одной подсети; разрешите Python в брандмауэре; убедитесь, что на роутере не включена изоляция клиентов.
- **Порт занят?** Программа автоматически пробует порты после 45431; UDP 45455 используется только для поиска.
- **Можно ли скачать файл, если отправитель выключил компьютер?** Да — если файл есть у кого-то из участников в сети.
- **Удаляются ли файлы при выходе из группы?** Нет, скачанные файлы остаются на компьютере.
- **Имя не меняется?** Значит, вы уже меняли его один раз — нужно больше 5 голосов «за»; при малом числе участников уменьшите `rename_vote_need` в `config.json`.
- **Потеряется ли личное сообщение, если получатель не в сети?** Нет — оно шифруется и хранится у участника в сети, а затем доставляется автоматически.
- **Обновление клонированием не работает?** Проверьте наличие Git (`git --version`), поле `update_repo` и доступ к GitHub.
- **Как корректно закрыть программу?** Просто закройте окно — всё сохраняется автоматически.

### 3.11 Разработка и тесты

```bash
python test_protocol.py    # тесты протокола: чат, личные сообщения, секретная пересылка, файлы, офлайн-поиск, голосование
python test_gui_smoke.py   # дымовые тесты GUI, проверка обновлений, разбор манифеста, клонирование (резерв/возврат)
```

- Тесты используют каталоги `_testdata/` и `_testdata_gui/` и не трогают вашу папку `data/`;
- Для ускорения тестов есть переменные окружения: `MCTIER_PRES_INTERVAL`, `MCTIER_PRES_TTL`, `MCTIER_VOTE_NEED`, `MCTIER_VOTE_WAIT`, `MCTIER_DATA_DIR`, `MCTIER_UPDATE_ROOT`.

### 3.12 Контакты

Ошибки, предложения и пожелания присылайте на **3814859587@qq.com**.

---

## 四、English

### 4.1 About the project

MCTier LAN Group ("Class Group Helper") is a LAN chat + group file-sharing application written in
**pure Python standard library** — no third-party dependencies, just copy the folder and run.

- **Zero dependencies**: only `socket / threading / tkinter / hashlib / hmac / json / urllib / subprocess` are used.
- **No server**: there is no central server. Every computer is a peer; whoever creates a group hosts its number, others join with the 6-character group ID.
- **WeChat 4.x-like desktop UI**: a narrow icon rail on the left (chat / log), the group list in the middle (search, unread badges, right-click menu), and the bubble chat area on the right (WeChat green, round avatars, group files, member list).
- **Built for classrooms**: homework exchange, announcements and chat inside one LAN — no internet required.

### 4.2 Requirements (read this first)

| Item | Requirement |
| --- | --- |
| Python | **3.8 or newer** (3.10+ recommended). The official Python installer **already includes tkinter** |
| Operating system | **Windows 10 / 11 (recommended, best experience)**; Windows 7 / 8.1, Linux and macOS also work |
| Third-party packages | **None** — no `pip install` needed at all |
| Network | All computers must be on the **same LAN / same subnet** (same router, switch or campus network) and **UDP broadcast** must be allowed |
| Ports | UDP **45455** (broadcast/discovery), TCP **45431** (file transfer, private-message relay; the program automatically tries up to 20 further ports if busy) |
| Firewall | On first run Windows asks for permission — click **Allow** (at least for private networks), otherwise nobody can see you |
| Privileges | A normal user account is enough; **no administrator rights** required |
| Disk usage | The program itself is < 200 KB; chat history and file cache live in `data/` and grow with your files |
| Git (optional) | Needed **only** for the "clone update" feature |

**Platform notes:**

- **Windows (recommended)**: native balloon notifications (`Shell_NotifyIcon`) and Windows 11 rounded corners.
  Double-click `启动.bat` to start (it prefers `pythonw`, so no black console window stays open).
- **Linux**: install tkinter (`sudo apt install python3-tk` on Debian/Ubuntu, `sudo dnf install python3-tkinter` on Fedora);
  notifications fall back to an in-app dialog; files open with `xdg-open`.
- **macOS**: use the official python.org build (it bundles tkinter); notifications fall back to a dialog; files open with `open`.
- **Network caveat**: if the school Wi-Fi enables AP/client isolation, broadcast packets are dropped and peers
  cannot see each other — use a single router/switch or a normal Wi-Fi network instead.

### 4.3 Features

| Feature | Description |
| --- | --- |
| Group chat | Real-time group messaging with bubbles (yours green on the right, others white on the left), previews and unread badges |
| Private chat | Click a member's avatar in the group (or an avatar in the chat) to open a private window; several windows can be open at once |
| Offline secret relay | If the recipient is offline, the message is **encrypted** and parked on an online classmate's computer; it is delivered automatically once the recipient appears, integrity-checked, then deleted from the relay |
| Group files | Send files to a group; members **download automatically**; double-click opens the file with the system default app |
| Offline file fallback | If the sender is offline, the app asks publicly in the log and any peer that already has the file serves it automatically |
| Rename voting | Free choice of name; the **first rename is free**, later ones need more than 5 approving votes |
| Public log | A read-only, network-wide public log visible to everyone |
| Update check | Checks `update.wenyi` when online, shows "New version — (number)" plus the changelog, and can update by cloning |
| System notifications | On download completion: `「qqq」群「AA」的「你好」下载完毕` |

### 4.4 Quick start

1. Install **Python 3.8+** on every computer (on Windows tick *Add Python to PATH*).
2. Copy the whole folder to each computer (USB drive, cloud drive or shared folder — the path may differ).
3. On Windows double-click **`启动.bat`**; elsewhere run `python mctier_lan.py`.
4. **Create a group**: "＋ 创建群组", enter a name (e.g. *Class 3-2*); a **6-character group ID** is generated.
5. **Join a group**: "加入群组" scans the LAN for discovered groups — select one, or type the group ID manually.
6. **Rename yourself**: click the avatar at the bottom-left (or "设置") and type a new name. **The first rename applies instantly**; later ones start a vote.
7. **Chat**: pick a group, type and press Enter.
8. **Private chat**: switch to the "成员" tab and click a classmate's avatar (or click an avatar inside the chat).
9. **Send files**: click "📎 发送文件", choose files; all group members download them automatically and get a system notification.
10. **Open files**: switch to "群文件" and double-click a file to open it with the system default application.

> Tip: share the group ID; one computer creates the group and everybody joins the same one.

### 4.5 Rename voting rules

- **First rename**: saved directly in Settings, effective immediately, no approval needed.
- **Second and later**: clicking "保存名字" broadcasts a voting request; every other online user gets a
  dialog "X wants to change the name from A to B — do you agree?" with 同意 / 拒绝 buttons.
- **Passing condition**: **more than 5 approving votes** (default; change `rename_vote_need` in `data/config.json`).
  If not enough votes arrive within **120 seconds**, the request fails and can be started again.
- **Anti-cheating**: one computer counts once per vote, and each vote has a unique ID.
- Requests and results are written to the public log for everyone to see.

### 4.6 Private chat and offline secret relay

**How to open**: the "成员" (members) tab in a group, or any avatar/name inside the chat area.

**Recipient online**: the message is addressed, so only the recipient sees it — no UI, no log entry for anyone else.

**Recipient offline (secret relay)** — fully automatic:

1. your app **encrypts** the message with a random key (a fresh key per message) and hands it to an online classmate C;
2. **C notices nothing**: no popup, no log, nothing in the UI — only ciphertext on disk;
3. as soon as recipient B comes online (heartbeat broadcast), C's program sends the encrypted packet directly to B;
4. B decrypts it and verifies the **HMAC integrity signature**; only on success does it acknowledge, and C then
   **deletes** the stored copy. If verification fails, the packet is kept and retried (up to 3 times);
5. on B's side the message looks like a normal private message, tagged "离线转存送达" (delivered from offline relay).

> Note: the relay uses a standard-library stream cipher plus HMAC — "machine-level" secrecy and integrity checking
> (the relay user cannot view it in the app). Resisting a relay owner who inspects raw disk files would require
> asymmetric encryption.

### 4.7 How group files are distributed

1. **Sender online** → the receiver downloads **directly** from the sender (SHA-256 verified, retried on corruption).
2. **Sender offline** → a **public request** is posted in the log (everyone can read it, only programs act on it):
   peers that already have the file answer with their LAN IP, and the requester takes the file from the
   **first responder** — no user interaction needed on either side.
3. Joining a group syncs that group's file list, so previously missed files are downloaded automatically.

### 4.8 Versioning and updates

**Manifest file `update.wenyi`** (shipped next to the program; a copy is published on GitHub / your website):

```
1.1.0
- Added private chat
- Added offline secret relay
```

- **Line 1 is the version number**; the remaining lines are the changelog (JSON is also supported:
  `{"version": "1.1.0", "notes": ["..."]}`).
- On startup (when online) the app fetches `update_url` (by default reads `update.wenyi` from GitHub);
  you can also trigger it manually via "设置 → 检查更新".
- **The app's own version also comes from line 1 of the local `update.wenyi`** — releasing a new version means
  editing just this one file.
- Same version → **silent**; request failed (offline) → **silent**; different version → dialog:

```
新版本 —— v1.1.1

更新内容：
- 修复了 xxx
- 新增了 yyy

（当前版本 v1.1.0）

"Yes" — update by cloning: the new version is cloned automatically and
all your session data is saved into the customer_god folder and carried over;
"No" — open the download page; "Cancel" — later.
```

**Clone update** runs in three automatic steps:

1. the current `data/` folder (**all of the user's session content**: groups, chats, file index, relay store)
   is backed up into a new folder **`customer_god/`** next to the program;
2. the latest version is fetched with a shallow `git clone` into **`update_clone/`**;
3. the content of `customer_god/` is **restored** into the new version's `data/` folder.

A dialog then shows the new path: close the old program, open the new folder and run `启动.bat` —
your chats and files are intact. Failures report a concrete reason (Git missing, repository not configured,
network unreachable).

**Settings (`data/config.json`)**:

| Field | Meaning |
| --- | --- |
| `update_url` | Manifest URL (already points to this repository's raw): `https://raw.githubusercontent.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-/main/update.wenyi` |
| `update_page` | Page opened by the "No" button (e.g. your GitHub Releases page) |
| `update_repo` | Repository used for the clone update: `https://github.com/ZZZ-need-sleep/Local_Network_Communication-created-by-DeepDeek-.git` |

> Note: these URLs already point to the repository above. If your default branch is `master` rather
> than `main`, change `main` in the code (or in `config.json`) to `master`.

### 4.9 Data directories

```
data/
├─ config.json          # settings: name, groups, ports, update URLs, rename-vote options
├─ index.json           # file index (hash -> name / sender / group)
├─ cache/<hash>          # file content cache (also used to serve files to others)
├─ downloads/<group>_<id>/   # auto-downloaded files, one folder per group
├─ secret_relay.json    # offline private messages relayed for others (ciphertext + signature), deleted after delivery
└─ secret_received.json # IDs of relays already received (deduplication)

customer_god/           # backup of "all user session content" made during a clone update
update_clone/           # the cloned new version (including its own data/)
update.wenyi            # version manifest: version number + changelog
```

### 4.10 FAQ

- **Two computers cannot see each other?** Make sure they are on the same subnet, allow Python through the
  firewall, and check that the router does not enable AP/client isolation.
- **Port already in use?** The program automatically tries ports after 45431; UDP 45455 is only used for discovery.
- **Can files still be downloaded after the sender shuts down?** Yes — as long as any classmate who already has
  the file is online, the file is fetched from them automatically.
- **Does leaving a group delete files?** No, downloaded files stay on your computer.
- **I cannot rename myself anymore?** You already renamed once; more than 5 classmates must approve. With few
  members, lower `rename_vote_need` in `config.json`.
- **Are private messages lost when the recipient is offline?** No — they are encrypted and parked on an online
  peer, then delivered automatically once the recipient appears.
- **Clone update fails?** Check that Git is installed (`git --version`), that `update_repo` is configured and
  that GitHub is reachable.
- **How do I quit cleanly?** Just close the window — all state is saved automatically.

### 4.11 Development and tests

```bash
python test_protocol.py    # headless protocol tests: group chat, private chat, secret relay, file download, offline fallback, rename voting
python test_gui_smoke.py   # GUI smoke tests + update check + manifest parsing + clone update (backup/restore)
```

- Tests use `_testdata/` and `_testdata_gui/` as data directories and never touch your `data/`;
- Environment variables for faster tests: `MCTIER_PRES_INTERVAL`, `MCTIER_PRES_TTL`, `MCTIER_VOTE_NEED`,
  `MCTIER_VOTE_WAIT`, `MCTIER_DATA_DIR`, `MCTIER_UPDATE_ROOT`.

### 4.12 Contact

Bug reports, suggestions and feature requests are welcome at **3814859587@qq.com**.
