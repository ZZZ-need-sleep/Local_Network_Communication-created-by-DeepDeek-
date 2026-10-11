# 03 · GUI 精确界面规格（tkinter → 纯 Win32 + GDI+ 移植用）

> 来源：`neiwang.py` 第 **2482–4760 行**（`FAMILY` / `EMOJI` 常量起，至 `main()` 结束）。
> 本文档描述的每一个数字、字符串、颜色**都直接来自源码**，不含推测。凡是源码里写了但实际未生效/未接线的行为，均以 `⚠️` 显式标注。
> 目标读者：没看过 Python、用 Win32 + GDI+（或任何原生 GUI）重写此界面的人。

---

## 0. 移植总览（必读）

- 这是**三栏 + 顶栏式**布局（仿微信 4.x 亮色主题），共 **5 个导航页**：聊天 / 内网 / 投票 / 更多 / 日志。
- 全部为**纯代码绘制**，没有图片资源、没有图标文件：头像用圆形色块 + 首字，图标用 Emoji 字符。
- 所有尺寸均为 **设备像素（Device Pixel）**，Tk 未做 DPI 感知；Win32 侧若要缩放，需自行 `SetProcessDpiAwareness` 后统一换算。
- Tk 的 `pack(side=?, padx, pady)` 语义：`padx=(左,右)`、`pady=(上,下)`，单值则四边相同。下文按此展开。
- Tk 的 `ipady=8` 表示**控件内部**上下各加 8 像素内边距（即总高 = 文本高 + 16），不是外边距。
- 窗口控件是**动态重建**的（Tk 里 `destroy()` 后重新 `pack`）。Win32 侧应改为「隐藏/复用控件 + 重绘」，但**可见状态必须与本文一致**。
- 事件驱动：核心（PeerCore）跑在后台线程，只往一个 `queue.Queue` 里投事件；GUI 每 **100 ms** 轮询一次队列（见 §4.0）。移植时保留「核心 → 队列 → UI 线程」的结构。

---

## 1. 调色板、字体、头像配色

### 1.1 调色板 `class P`（源码 2490–2505，全部为 sRGB 十六进制）

| 常量名 | 值 | 语义 / 用在哪 |
|---|---|---|
| `P.bg` | `#f7f7f7` | 主背景（右侧内容区、日志页以外的大面积底色），也是消息区底色 |
| `P.panel` | `#ffffff` | 面板白（左侧图标栏、群列表、头部、输入区、卡片） |
| `P.hover` | `#f2f2f2` | 悬停态背景（按钮按下态 `activebackground`） |
| `P.selected` | `#e8f7ee` | 选中态背景（选中的群卡片、选中的导航按钮、激活的 chip、列表选中行背景） |
| `P.accent` | `#07c160` | 微信绿（主按钮、当前 chip 文字、在线状态点、`_flat_btn` 前景） |
| `P.accent_hv` | `#06ad56` | 绿色按钮按下态 |
| `P.text` | `#191919` | 主文字 |
| `P.sub` | `#9a9a9a` | 次要文字（时间戳、说明、未激活 chip、灰色按钮文字） |
| `P.border` | `#e6e6e6` | 1px 分隔线 / 边框 |
| `P.bubble_self` | `#95ec69` | 自己发出的聊天气泡底色 |
| `P.bubble_other` | `#ffffff` | 别人发来的聊天气泡底色 |
| `P.badge` | `#f76260` | 未读红点 / 红点数字背景 / 「拒绝」按钮前景 / 红字提示 |
| `P.log_bg` | `#f5f8fc` | 日志页 `Text` 背景（淡蓝白） |
| `P.entry_bg` | `#f3f3f3` | 搜索框背景 |

**注意：文本输入框并不用 `P.entry_bg`。** 所有 `Entry`（搜索框、聊天输入、创建群对话框、设置对话框、加入群对话框的输入）**硬编码为 `#f7f7f7`**（与 `P.bg` 同值）。只有群列表顶部的搜索框用 `P.entry_bg = #f3f3f3`。

其他直接硬编码的颜色：
- `#ffffff`：头像首字、绿色/红色按钮上的文字、未读数字文字。
- 在 `P.panel` 底上用「实线 1px 边」的次级按钮（`_flat_btn`、`加入群组`、`重试下载` 等）。

### 1.2 字体

| 常量 | Windows 值 | 非 Windows 兜底 |
|---|---|---|
| `FAMILY` | `Microsoft YaHei UI` | `DejaVu Sans` |
| `EMOJI` | `Segoe UI Emoji` | `DejaVu Sans` |
| 日志等宽 | `Consolas` | `monospace` |

根窗口默认字体：`root.option_add("*Font", (FAMILY, 10))`——**未显式指定字体的控件用 Family 10pt 常规**。

源码中出现的所有 `(字体, 字号, 样式)` 组合（Tk 字号为**点**；`bold` 为加粗）：

| 字号 / 样式 | 使用处 |
|---|---|
| `FAMILY 8` | 导航栏按钮小标签、我的名字（图标栏底部）、任务栏 `我的名字`、群卡片右侧时间、群卡片未读数字（8 bold）、气泡下方时间戳（自己「MM-DD HH:MM:SS  我」、对方「MM-DD HH:MM:SS」）、对方名字（8 bold）、投票卡片「规则：…」行、投票剩余时间行、可改票提示、成员卡片 IP 行、「我自己」标签、内网页 `net_apply_lbl`、「更多」页底部灰字说明、设置对话框提示语、底部版本信息、创建群对话框底部说明（8）、私聊窗口在线状态（8）、私聊消息信息行（8） |
| `FAMILY 9` | 群卡片副标题「点击进入群聊」/ 预览、群头部 `hd_info`、chip 未激活时不是 9（见下）、状态栏 `status_lbl`、顶部「退出群组」按钮、日志页「清空本机显示」按钮、内网页 `net_status_lbl`、`net_info`、`_flat_btn` 文字、投票卡片状态标签（9 bold）、投票卡片信息行、投票卡片按钮（9 bold）、投票卡片说明行、成员卡片「私聊 →」、群文件页三个按钮、投票页说明、投票页「我已投：…」（9 bold）、设置对话框若干、加入群对话框说明、内网页两个 Treeview 的 heading 字体（10 bold 见下）、`更多`页 `hist_tree` 列宽说明等 |
| `FAMILY 10` | 全局默认、群文件/加入群 Treeview 行、内网 Treeview 行（`Net.Treeview` 用 10）、chip 未激活（10）、chip 激活（10 bold）、内网页「成员登记表」标题（11 bold 见下）、搜索框、聊天输入框、发送按钮（10 bold）、聊天气泡正文、成员卡片名字（10 bold）、群菜单（10）、对话框正文/按钮大部分为 10 / 10 bold、私聊窗口输入与发送 |
| `FAMILY 11` | 投票悬浮面板标题（11 bold）、投票卡片申请标题（11 bold）、内网页两个区块标题（11 bold）、投票页分组标题（11 bold）、改名投票对话框「从「旧」改为「新」」（11）、创建群对话框输入框（11）、设置对话框输入框（11）、空状态按钮（11 bold）、内网引导页按钮（11 bold）、`net_title`（13 bold 见下）、私聊窗口顶部名字（11 bold）、私聊气泡正文（10 见上） |
| `FAMILY 12` | 改名投票对话框首行（12 bold） |
| `FAMILY 13` | 各页页头标题（13 bold：内网 / 投票 / 更多 / 公共日志）、`net_title`（13 bold）、空状态「选择左侧群组开始聊天」（13）、内网引导页主提示（13 bold） |
| `FAMILY 14` | 群头部群名（14 bold）、创建群 / 设置对话框标题（14 bold）、加入群对话框标题（14 bold） |
| `FAMILY 15` | 群列表标题「群聊」（15 bold）、图标栏 logo「群」（15 bold） |
| `EMOJI 16` | 左侧导航栏 5 个图标 |
| `EMOJI 44` | 内网引导页大图标「📡」 |
| `EMOJI 46` | 空状态大图标「💬」 |
| `FAMILY 13`（非 bold） | 图标栏底部「设置」齿轮为 `FAMILY 13`，文字「设置」为 `FAMILY 8` |

### 1.3 头像配色

```python
AVATAR_COLORS = ["#07c160", "#576b95", "#f76260", "#10aeff", "#ffc300",
                 "#8a2be2", "#ff9f40", "#00b0b9", "#e062a4", "#5f9ea0"]
```

**取色算法（必须逐字复刻）：**

```
index = (Σ 每个字符的 Unicode 码点) mod 10
name 为空/None 时先用 "?" 代替
color = AVATAR_COLORS[index]
```

**头像绘制（`Avatar`，源码 2516–2526）：**

- 画布尺寸 `size × size`，背景 = 父控件背景色。
- 实心圆：`ellipse(1, 1, size-1, size-1)`，**填充色 = 描边色 = 上面算出的颜色**（即直径 = `size - 2`，四周留 1px）。
- 居中文字：`(name or "?")[:1]`——**只取第一个字符**，白色 `#ffffff`，字体 `(FAMILY, max(9, int(size * 0.38)), bold)`。
  - size=36 → 字号 `max(9, 13) = 13` bold
  - size=40 → `max(9, 15) = 15` bold
  - size=34 → `max(9, 12) = 12` bold
  - size=30 → `max(9, 11) = 11` bold

**头像各处使用尺寸（硬编码）：**

| 位置 | 直径 |
|---|---|
| 群卡片（群列表） | **40** |
| 图标栏底部「我的头像」 | **36** |
| 成员列表卡片 | **36** |
| 群聊里对方气泡上方的小头像 | **30** |
| 私聊窗口头部头像 | **34** |
| 私聊消息里对方小头像 | **30** |

---

## 2. 主窗口与布局（所有像素数字）

### 2.1 窗口

| 项 | 值 |
|---|---|
| 标题 | `{APP_NAME} v{APP_VERSION}  ·  局域网群组`（APP_NAME=`内部网`，APP_VERSION=`1.1.0`）→ 实际字符串 `内部网 v1.1.0  ·  局域网群组`（**点号两侧各两个空格**） |
| 默认尺寸 | `1180 × 740` |
| 最小尺寸 | `980 × 620` |
| 根背景 | `P.bg = #f7f7f7` |
| 关闭行为 | `WM_DELETE_WINDOW` → `_on_close()`：先隐藏投票悬浮窗，再 `core.stop()`，然后销毁窗口 |
| Win11 圆角 | 只在 Windows 上执行：取根窗口句柄，`DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, DWMWCP_ROUND=2, 4)`。失败静默忽略。**对话框不做圆角。** |

### 2.2 ASCII 布局总览

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ shell (P.bg, 填满窗口)                                                                │
│ ┌────────┬──────────────────────┬──────────────────────────────────────────────────┐ │
│ │ rail   │ listp                │ right_main  (或 right_net / right_vote /          │ │
│ │ 宽 64  │ 宽 300               │              right_more / right_log)             │ │
│ │(P.panel│ (P.panel)            │  (P.bg)                                          │ │
│ │ )      │                      │                                                  │ │
│ │ ┌────┐ │ ┌──────────────────┐ │ ┌──────────────────────────────────────────────┐ │ │
│ │ │ logo│ │ │ 群聊  (15 bold)  │ │ │ header  高 64  (P.panel)                     │ │ │
│ │ │「群」│ │ └──────────────────┘ │ │ 群名(14b) 群号·在线   [聊天|群文件|成员] [退出]│ │ │
│ │ └────┘ │ ┌────────┬─────────┐ │ └──────────────────────────────────────────────┘ │ │
│ │  💬聊天 │ │＋创建群组│ 加入群组 │ │ ┌──────────────────────────────────────────────┐ │ │
│ │  📡内网 │ └────────┴─────────┘ │ │ viewbox (P.bg, 填满剩余高度)                  │ │ │
│ │  🗳投票 │ ┌──────────────────┐ │ │   chat / files / members / netgate / empty   │ │ │
│ │  ⋯更多  │ │ 🔍  搜索群组      │ │ │                                              │ │ │
│ │  📋日志 │ └──────────────────┘ │ │                                              │ │ │
│ │ (弹性)  │ ┌──────────────────┐ │ └──────────────────────────────────────────────┘ │ │
│ │        │ │ 群卡片列表        │ │ ┌──────────────────────────────────────────────┐ │ │
│ │ (头像)  │ │ (可滚动)          │ │ │ status_bar 高 26 (P.panel, 贴底)              │ │ │
│ │ 我的名字│ │                  │ │ │                        状态文字(9, 右对齐,12px)│ │ │
│ │ ⚙设置  │ │                  │ │ └──────────────────────────────────────────────┘ │ │
│ └────────┴─┴──────────────────┴─┴──────────────────────────────────────────────────┘ │
│      ↑1px 右边框        ↑1px 右边框                                                   │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

### 2.3 第一栏：左侧窄图标导航栏（源码 2684–2727）

- 容器 `rail`：宽 **64**，`side=left`，纵向填满，`pack_propagate(False)`（**子控件不撑大它**）；背景 `P.panel`。
- rail 右缘一条 1px 竖线 `P.border`（`side=right, fill=y`）。

| 元素 | 位置/尺寸 | 说明 |
|---|---|---|
| Logo | `pady=(14, 12)`，文字「群」，`bg=P.accent`，`fg=#ffffff`，`FAMILY 15 bold`，控件 `width=2`、`pady=4` | 即绿色小方块，宽度由 2 个字符宽决定 |
| 5 个导航按钮 | 每个 `pady=3`、`ipadx=2`（即左右各 2px 内边距） | 见下表 |
| 导航按钮内部 | 上：Emoji 图标 `EMOJI 16`；下：文字 `FAMILY 8`，`fg=P.sub` | 两行垂直居中 |
| 弹性占位 | `_rail_pad` 填满剩余高度 | 把下面元素压到底部 |
| 我的头像 | `Avatar(36)`，`pady=(0, 2)`，点击打开设置 | |
| 我的名字 | `self.cfg["name"][:4]`（**截前 4 个字符**），`FAMILY 8`，`fg=P.sub`，`pady=(0,6)`，`padx=2` | |
| 设置按钮 | 齿轮「⚙」`FAMILY 13` + 文字「设置」`FAMILY 8`，均 `fg=P.sub`，整体 `pady=(0,12)`，点击打开设置 | |

**导航按钮表（顺序即显示顺序）：**

| key | 图标字符 | 文字 |
|---|---|---|
| `chat` | `💬` | `聊天` |
| `net` | `📡` | `内网` |
| `vote` | `🗳` | `投票` |
| `more` | `⋯` | `更多` |
| `log` | `📋` | `日志` |

**选中态绘制：** `switch_nav(key)` 把该按钮所有**子控件**背景设为 `P.selected`，其余按钮的子控件背景设回 `P.panel`。注意：**按钮外框本身（`b` frame）背景从不改**（源码只遍历 `b.winfo_children()`），因此选中态实际表现为「图标+文字块的绿色底」。移植时可实现为整块 64×~44 的浅绿高亮——视觉结果一致。

**投票红点（`vote_dot`，源码 2707–2712）：**
- 一个 11×11 的画布，画 `ellipse(1,1,10,10)`——**直径 9**，填充+描边 `P.badge`。
- 仅在 `_update_vote_badge()` 时定位：`place(relx=1.0, x=-3, y=1, anchor="ne")` → 位于投票按钮**右上角**，相对按钮右边缘 x 偏移 **-3**、y = **1**，锚点为右上。
- 有「待我投票」的申请时出现，投完后隐藏。
- 悬停事件绑在按钮 frame 和它的图标、文字三个控件上（`Enter`/`Leave`，`add="+"` 不覆盖已有绑定）。

### 2.4 第二栏：群组（会话）列表（源码 2729–2770）

- 容器 `listp`：宽 **300**，`side=left`，纵向填满，`pack_propagate(False)`；背景 `P.panel`。
- 右缘 1px 竖线 `P.border`。

| 元素 | 数值 |
|---|---|
| 标题区 `head` | `padx=14`（左右各 14），`pady=(16, 8)`（上 16 下 8） |
| 标题「群聊」 | `FAMILY 15 bold`，`fg=P.text`，左对齐 |
| 按钮行 `btns` | `padx=12`，`pady=(0, 8)` |
| 「＋ 创建群组」 | 绿色实心：`bg=P.accent`、`fg=#ffffff`、按下 `P.accent_hv`、`relief=flat`、`bd=0`、`FAMILY 10 bold`、`pady=4`、左侧、横向填满并参与伸缩、右外边距 6 | 
| 「加入群组」 | 白底绿字实线框：`bg=P.panel`、`fg=P.accent`、`relief=solid`、`bd=1`、`FAMILY 10`、`pady=3`、`width=9`（9 字符宽）、右侧 |
| 搜索框 | `padx=12`、`pady=(2, 8)`、`ipady=5`（内上下各 5）、`bg=P.entry_bg=#f3f3f3`、`FAMILY 10`、无边框、插入符色 `P.text`；初始文本 `🔍  搜索群组`（**放大镜后跟两个空格**），初始前景 `P.sub` |
| 群卡片滚动容器 | 填满剩余高度 |

**搜索框行为：**
- 有 `trace_add("write")`：内容变化即调用 `_on_search()`；占位状态下不刷新。
- `FocusIn`：若是占位态 → 关闭占位、清空文本、`fg` 改 `P.text`。
- `FocusOut`：若内容去空格后为空 → 恢复占位态（清空、重新填 `🔍  搜索群组`、`fg=P.sub`）并刷新列表。
- 过滤规则：`q = 去掉首尾空格并转小写`；保留 `q in gname.lower()` 或 `q in gid.lower()` 的群。

**群卡片列表布局：** 每个卡片 `pack(fill="x", padx=8, pady=2)`（左右各 8，上下各 2）。

**群卡片 `GroupCard` 内部（源码 2568–2631）——这是本移植里最需要手绘的部分：**

```
┌─ 卡片 (P.panel / 选中 P.selected；高约 58) ──────────────────────────────┐
│  ┌────┐  群名称（11 bold, P.text）                        MM-DD HH:MM:SS │
│  │头像│  ┌                           ┐                     ┌───┐         │
│  │ 40 │  │ 点击进入群聊 或 "名字: 内容" │                     │ n │ ← 未读  │
│  └────┘  └ (9, P.sub)                ┘                     └───┘         │
└──────────────────────────────────────────────────────────────────────────┘
   ↑左10 右8                                                      右12
```

| 元素 | 精确数值 |
|---|---|
| 头像 | 直径 **40**，`padx=(10, 8)`（左 10 右 8），`pady=9`（上下各 9） |
| 中间列 | `side=left`，横向填满并伸缩 |
| 群名 | `FAMILY 11 bold`，`fg=P.text`，左对齐，`pady=(7, 0)`，横向填满 |
| 副标题 `sub` | 默认文本 `点击进入群聊`；有消息时由 `set_preview` 覆盖为 `"{发送者}: {文本}"`，**截取前 28 个字符**；若截取结果为空则显示 `…`；`FAMILY 9`，`fg=P.sub`，`pady=(1, 7)`，横向填满 |
| 右侧列 | `side=right`，`padx=12` |
| 时间 `time_lbl` | 文本 = `fmt_time(ts)[5:]`（`fmt_time` = `"%m-%d %H:%M:%S"`，取 `[5:]` 即 **`HH:MM:SS`**）；`FAMILY 9`，`fg=P.sub`，右对齐，`pady=(8, 0)` |
| 未读徽标 `badge` | 文本 = 数字，**≥100 时显示 `99+`**；`bg=P.badge`、`fg=#ffffff`、`FAMILY 8 bold`、`padx=5`、`pady=1`；为 0 时 `pack_forget`（隐藏）；有值时 `pack(anchor="e", pady=(4,0))` |
| 整体 | 光标手型；背景色同时刷到 `[自身, 头像, mid, sub, right, time_lbl]` 这 6 个控件上 |
| 选中态 | 背景 `P.selected`；未选中 `P.panel` |
| 悬停态 | `set_hover(True)` → 未选中时背景 `P.hover`。⚠️ **源码中没有任何地方调用 `set_hover`，也没有绑定 `<Enter>/<Leave>`** —— 即当前实现**没有悬停变色**。移植时建议不实现悬停，或在 Win32 上补 `WM_MOUSEMOVE`/`TrackMouseEvent` 实现 `#f2f2f2`。 |
| 左键单击 | `_bind_click` 递归绑定到卡片**及所有子控件**（含头像、名字、副标题、时间、徽标）→ `app.select_gid(gid)` |
| 右键单击 | 仅绑在卡片 frame 上 → `app.show_group_menu(gid, e.x_root, e.y_root)`（**屏幕坐标**） |

**右键菜单（`show_group_menu`）：** Tk 原生菜单（需自绘成无边框 popup），内容：
1. 禁用项：`群：{gname}（{gid}）`
2. 分隔线
3. `退出该群组` → `_leave_group_by(gid)`

### 2.5 第三栏：右侧主区（群聊页，源码 2772–2841）

`right_main`：`side=left`，横向纵向填满并伸缩，背景 `P.bg`。

**a) 群头部 `header`：高 64，`P.panel`，`pack_propagate(False)`，底部一条 1px `P.border` 横线。**

| 元素 | 数值 |
|---|---|
| `hd_name` 群名 | `FAMILY 14 bold`，`fg=P.text`，`side=left`，`padx=(18, 0)`，`pady=(6, 0)` |
| `hd_info` | `FAMILY 9`，`fg=P.sub`，`side=left`，`padx=8`，`pady=(10, 0)`；文本 `群号 {gid} · 在线 {len(members)} 人` |
| chips 栏 | `side=right`，`padx=16`，`pady=(10, 0)` |
| 3 个 chip | 每个 `side=left`、`padx=2`、内边距 `pady=4`/`padx=14`、光标手型 |
| chip 文字 | `聊天` / `群文件` / `成员` |
| chip 非激活 | `FAMILY 10`、`fg=P.sub`、`bg=P.panel` |
| chip 激活 | `FAMILY 10 bold`、`fg=P.accent`、`bg=P.selected` |
| 「退出群组」按钮 | `side=right`，`padx=(0, 14)`，`pady=(16, 0)`，`FAMILY 9`，`fg=P.sub`，按下背景 `P.hover`，无边框 → `_on_leave_group()` |

**b) 视图容器 `viewbox`：** 填满头部与状态栏之间的剩余空间，背景 `P.bg`。它是 **5 个互斥子视图的父容器**：`view_netgate`（未入网引导）、`view_empty`（未选群）、`view_chat`、`view_files`、`view_members`。任一时刻**只有一个** `pack(fill="both", expand=True)`。

**视图选择逻辑 `_raise_view(key)`（源码 3022–3040，必须严格按此顺序判断）：**

1. `core.in_net()` 为假 → 渲染并显示 **netgate**（未入网引导页），结束。
2. `current_gid` 为空 → 显示 **empty**（空状态页），结束。
3. `key == "chat"` → 显示 chat 视图，并 `after(60ms)` 把消息列表滚到底。
4. `key == "files"` → 先 `_render_files()` 再显示 files 视图。
5. `key == "members"` → 先 `_render_members()` 再显示 members 视图。

**c) 状态栏 `status_bar`：高 26，`P.panel`，贴底（`side=bottom`），顶部一条 1px `P.border`。**

- `status_lbl` 文本：`{net_txt} · 在线用户 {n} · v{APP_VERSION}`
  - 已入网：`net_txt = 内网「{name}」{member_count} 人`
  - 未入网：`net_txt = 未加入内网（正在扫描）`
  - `n` = `peers` 中 `now - ts <= PRESENCE_TTL(30s)` 的数量
- 样式：`FAMILY 9`、`fg=P.sub`、右对齐、`padx=12`。

### 2.6 日志页（右侧区域，源码 2823–2840）

- 容器 `right_log`：背景 `P.panel`，`side=left`，填满并伸缩。
- 页头：高 **48**，`pack_propagate(False)`；左侧标题 `局域网公共日志（所有人可见 · 只读）`（`FAMILY 13 bold`，`fg=P.text`，`padx=18`）；右侧按钮 `清空本机显示`（`FAMILY 9`，`fg=P.sub`，无边框，按下 `P.hover`，`padx=14`）。
- 页头下方 1px `P.border` 横线。
- 日志正文：`Text`，`wrap="word"`（按词换行）、只读（`state="disabled"`，写入时临时置 normal）、`bg=P.log_bg=#f5f8fc`、无边框、字体 `Consolas 9`（非 Windows 为 `monospace 9`），`side=left` 填满。
- 右侧竖直滚动条：**Tk 默认滚动条**（未指定 width），`side=right, fill=y`。
- 追加行为 `_append_log(line)`：直接插入 `line + "\n"`；**最多保留 `LOG_MAX_LINES = 800` 行**，超出时从**开头**删掉多余行；随后滚动到末尾（`see("end")`）。
- 「清空本机显示」= 删除 `1.0` 到 `end`（只清本机显示，不影响别人）。

---

## 3. 各页面控件树与绘制细节

### 3.1 群聊视图 `view_chat`（源码 3504–3534）

```
view_chat (P.bg)
├── chat_scroll (ScrollFrame, P.bg)  ← fill=both, expand=True
│   └── inner → msg_bg (Frame, bg=P.bg)  ← 消息条容器，fill=both expand=True
└── 输入区 inp (P.panel, side=bottom, fill=x)
    ├── 1px P.border 横线（side=top）
    ├── toolbar (padx=12, pady=(6,2))
    │   └── 「📎  发送文件」按钮 (P.panel, fg=P.sub, flat, FAMILY 10, 左对齐)
    └── row (padx=12, pady=(0,12))
        ├── 输入框 entry  (bg=#f7f7f7, FAMILY 10, 左, 横向填满+伸缩, ipady=8, 右外边距 10)
        └── 「发送」按钮  (P.accent/#ffffff, 按下 P.accent_hv, flat, FAMILY 10 bold,
                          width=9, pady=8, 右侧)
```

> ⚠️ 源码注释把 `msg_bg` 写成「白色卡片」，但实际 `bg=P.bg`（`#f7f7f7`）。**消息区底色是 `#f7f7f7`**，气泡才是白色/绿色。

**消息条绘制 `_msg_kind_row(m)`（源码 3539–3581）——逐条精确规格：**

**系统消息**（`m["t"] == "sys"`）：
- 居中单行文本 `── {text} ──`（前后各两个 U+2500「─」）
- `FAMILY 9`，`fg=P.sub`，`pady=3`，水平居中。

**聊天消息（共同）：** 外层 `row`：`fill="x"`，`padx=16`，`pady=5`。

**自己发的（`m["self"]` 为真）：**
- `box` 靠**右**（`side="right"`、`anchor="e"`）
- 气泡：`bg=P.bubble_self=#95ec69`、`fg=P.text`、**`wraplength=430`**（最大宽度 430）、左对齐、内边距 `padx=12 / pady=8`、`FAMILY 10`、靠右
- 气泡下方：`{fmt_time(ts)[5:]}  我` → **`HH:MM:SS  我`**（时间与「我」之间**两个空格**），`FAMILY 8`、`fg=P.sub`、右对齐、`pady=(2,0)`
- **没有头像、没有名字**

**别人发的：**
- `box` 靠**左**（`side="left"`、`anchor="w"`）
- 上方一行 `top`（靠左）：
  - `Avatar(30)`，`padx=(0, 6)`
  - 右侧名字块：上行 `str(m["from"])`（`FAMILY 8 bold`、`fg=P.sub`、左对齐），下行 `fmt_time(ts)[5:]`（`HH:MM:SS`，`FAMILY 8`、`fg=P.sub`、左对齐）
  - **若消息带 `from_uid`**：头像和名字的光标变手型，并绑定左键 → `open_dm(from_uid)`（快速私聊）
- 气泡：`bg=P.bubble_other=#ffffff`、`fg=P.text`、`wraplength=430`、左对齐、`padx=12 / pady=8`、`FAMILY 10`；`pack(anchor="w", padx=(0, 40), pady=(2,0))` → **右侧留 40px 空**（让左侧气泡不贴右边缘）

**增量绘制：** `_render_chat(append)` 用 `self._rendered_n` 记住已渲染条数，只渲染 `lines[_rendered_n:]`。全量重建（切群时）会先销毁所有子控件并把计数归零。追加后 `scroll_bottom_later()`（60ms 后滚到底）。

**滚动容器 `ScrollFrame` 规格（源码 2529–2558）：**
- 内部 = `Canvas`（无边框、无高亮）+ 右侧竖直 `Scrollbar`，**滚动条宽 8**（`width=8`），`side=right, fill="y"`。
- `Canvas` 里放一个 `inner` 窗口，`anchor="nw"`，宽度始终跟随画布宽度（`<Configure>` 时 `itemconfigure(width=e.width)`）——**即内部内容永远与可视宽度等宽，不出现横向滚动**。
- 内容尺寸变化时更新 `scrollregion = bbox("all")`。
- 鼠标滚轮：仅当指针**进入**该画布时全局绑定 `<MouseWheel>`，**离开**时解除。滚动量 = `int(-e.delta / 120)` 个「单位」（Windows 上 delta 为 ±120 的倍数 → 每格滚 1 单位）。
- `scroll_bottom()`：先 `update_idletasks()`，再 `yview_moveto(1.0)`（滚到最底）。

### 3.2 空状态视图 `view_empty`（源码 3487–3501）

以 `place(relx=0.5, rely=0.42, anchor="center")` 定位一个垂直居中的块（**水平居中，垂直位于容器 42% 处**）：

| 顺序 | 内容 | 样式 |
|---|---|---|
| 1 | `💬` | EMOJI，字号 **46** |
| 2 | `选择左侧群组开始聊天` | `FAMILY 13`，`fg=P.sub`，`pady=(8,4)` |
| 3 | `或创建一个群组，邀请同学加入` | `FAMILY 10`，`fg=P.sub` |
| 4 | 「＋ 创建群组」按钮 | `P.accent`/`#ffffff`，按下 `P.accent_hv`，flat，`FAMILY 11 bold`，`padx=22`，`pady=6`，`pady=14` 外边距 → `_on_create_group()` |

### 3.3 未入网引导视图 `view_netgate`（源码 3043–3064）

同样 `place(relx=0.5, rely=0.42, anchor="center")`：

| 顺序 | 内容 | 条件与样式 |
|---|---|---|
| 1 | `📡` | EMOJI **44** |
| 2 | 主标题 | 若 `core.net_applying` 为假：`正在扫描附近的内网…`；否则 `已提交入网申请，等待成员投票…`；`FAMILY 13 bold`，`fg=P.text`，`pady=(8,4)` |
| 3 | 明细 | `target = (core.net_applying or {}).get("name") or ""`；target 非空 → `目标内网「{target}」· 投票 1 天内有效`；否则 → `发现附近内网会自动申请加入；没有则自动建立自己的内网`；`FAMILY 10`，`fg=P.sub` |
| 4 | 投票计数（条件） | 若 `core.net_open_votes()` 非空：`当前投票：{n} 项（左侧「投票」图标可查看）`，`fg=P.badge`，`FAMILY 10 bold`，`pady=(10,0)` |
| 5 | 「打开内网页面」按钮 | `P.accent`/`#ffffff`，flat，`FAMILY 11 bold`，`padx=22`，`pady=6`，`pady=14` → `switch_nav("net")` |

### 3.4 内网页 `right_net`（源码 2846–2919 + `_render_net_page` 3067–3111）

**页头：** 高 **48**，`P.panel`；左侧标题 `内网`（`FAMILY 13 bold`，`padx=18`）；紧邻右侧 `net_status_lbl`（`FAMILY 9`、`fg=P.sub`、`padx=6`），文本由 `net_status` 事件驱动，**最长截断 80 字符**。页头下 1px `P.border` 线。

**主体 `body`：** `padx=16`，`pady=12`，`fill=both expand=True`，背景 `P.bg`。

**a) 我的内网信息卡 `net_card`（`P.panel`，横向填满）：**

| 元素 | 数值 |
|---|---|
| `net_title` | `FAMILY 13 bold`，`fg=P.text`，左对齐，`padx=16`，`pady=(12,2)` |
| `net_info` | `FAMILY 9`，`fg=P.sub`，左对齐（`justify=left`），`padx=16`（无下边距） |
| 按钮行 `nbar` | `padx=16`，`pady=(10,12)`；4 个 `_flat_btn` 依次 `side=left`，前 3 个右外边距 8，最后一个无 |
| 按钮 1 | `重新扫描附近内网` → `_on_net_scan` |
| 按钮 2 | `自行建立内网` → `_on_net_create` |
| 按钮 3 | `重命名` → `_on_net_rename` |
| 按钮 4 | `退出内网` → `_on_net_leave` |

**`_flat_btn` 统一样式：** `bg=P.panel`、`fg=P.accent`、按下背景 `P.selected`、按下前景 `P.accent`、`relief=solid`、`bd=1`、手型、`FAMILY 9`、`padx=10`、`pady=3`。

**卡片文本与按钮可用性（按 `core.net_view()` 结果三分支）：**

1. 已入网（`v["in_net"]` 真）：
   - `net_title = 我的内网：{v['name']}（编号 {v['nid']}）`
   - `role = "发起者" if v["is_founder"] else "成员"`
   - `net_info = 成员 {member_count} 人（当前在线 {online_count} 人） · 我是{role} · 我的物理地址 {my_mac_pretty}`
   - 按钮：`自行建立内网` **disabled**；`退出内网` **normal**；`重命名` normal（是发起者）/ **disabled**（否则）
2. 未入网、且 `v["applying"]` 非空：
   - `net_title = 正在申请加入内网「{applying.name or applying.nid}」`
   - `net_info = 已提交入网申请，等待成员投票（有效期 1 天）。投票进度见左侧「投票」图标。`
   - 按钮：建立 **normal**、退出 **disabled**、重命名 **disabled**
3. 未入网、无申请：
   - `net_title = 尚未加入任何内网`
   - `net_info = 程序会自动扫描附近的内网：有就申请加入，没有就自行建立一个。`
   - 按钮：建立 **normal**、退出 **disabled**、重命名 **disabled**

**b) 「成员登记表（物理地址 / 内网 IP）」** 标题：`FAMILY 11 bold`，`fg=P.text`，左对齐，`pady=(16,6)`。

- 表格：`ttk.Treeview`，`show="headings"`，**`height=7`（7 行高）**，样式 `Net.Treeview`：行高 **30**、字体 `FAMILY 10`、背景/字段背景 `P.panel`；表头 `FAMILY 10 bold`、背景 `P.panel`、前景 `P.sub`；选中行背景 `P.selected`、前景 `P.text`。
- 列（顺序固定，均左对齐）：

| 列 id | 表头 | 宽 |
|---|---|---|
| `name` | `名字` | 150 |
| `mac` | `物理地址（MAC）` | 190 |
| `ip` | `内网 IP` | 140 |
| `joined` | `加入时间` | 140 |
| `role` | `角色` | 90 |

- 行数据（5 列依次为）：`{m['name']}` + （若 `m['mac'] == cfg['uid']` 则追加 `（我）`）；`fmt_mac(m['mac'])`；`m['ip'] or "-"`；`fmt_time(m['joined_ts'] or 0)`；`m['founder']` 为真则 `发起者`，否则 `成员`
- 每次渲染先清空全部行再重填。

**c) 「附近的内网（自动扫描发现）」** 标题：`FAMILY 11 bold`，`pady=(14,6)`。

- 表格：`ttk.Treeview`，`show="headings"`，**`height=5`**，同样式。
- 列：

| 列 id | 表头 | 宽 |
|---|---|---|
| `name` | `内网名称` | 220 |
| `nid` | `内网编号` | 110 |
| `founder` | `发起者物理地址` | 190 |
| `count` | `成员数` | 90 |

- 行的 `iid` = `n["nid"]`（用编号作为行标识，**用 iid 直接取选中项**）；插入失败被 `try/except` 吞掉。
- 表格下方 `pady=(0,6)`，再一行 `nbar2`：`申请加入所选内网` 的 `_flat_btn`（左）+ `net_apply_lbl`（`FAMILY 9`、`fg=P.sub`、`padx=10`）。
- `net_apply_lbl` 文案：已入网且有附近内网 → `（已在其他内网中，不能重复申请）`；附近为空 → `附近暂未发现其它内网`；其他情况保持上次文本（例如 `_on_net_scan` 会先置为 `正在扫描附近的内网…`）。

### 3.5 投票页 `right_vote`（源码 2921–2933 + `_render_vote_page` 3208–3232）

**页头：** 高 **48**；标题 `投票`（13 bold，`padx=18`）+ 灰字 `入网申请投票统一在这里处理（可弃权；投出后 5 分钟内可改票）`（9，`P.sub`）；下面 1px 线。

**主体：** `ScrollFrame`，`padx=16`，`pady=12`，填满。每次渲染先销毁内部所有子控件，然后按顺序：

1. `待我投票（{len(cards)}）` — `FAMILY 11 bold`，`fg=P.text`，左对齐；`cards = core.net_pending_for_me()`
2. `_render_vote_cards(cards)`（见 §3.7）
3. 若存在「其它进行中的投票」：`opened = [c for c in core.net_open_votes() if c.req_id 不在 cards 的 req_id 集合]`，非空时：
   - `其它进行中的投票（{len(opened)}）` — 11 bold，`pady=(16,0)`
   - `_render_vote_cards(opened)`
4. 若 `core.net_applying` 非空：
   - `我发起的入网申请` — 11 bold，`pady=(16,0)`
   - `mine = [c for c in core.net_open_votes() if c.req_id == applying.req_id]`；渲染卡片
   - `撤回申请` 的 `_flat_btn`（`anchor="w", pady=8`）→ `core.net_cancel_apply()`
5. 说明文字（`FAMILY 9`、`fg=P.sub`、左对齐、**`wraplength=760`**、`pady=(16,0)`）：
   `说明：可弃权；投出后 5 分钟内可改票；一次投票有效期为 1 天，过期视为投票结束。历史投票在左侧「更多」里查看。`

### 3.6 更多页 `right_more`（源码 2935–2966 + `_render_more_page` 3235–3249）

**页头：** 高 **48**，标题 `更多`（13 bold，`padx=18`）+ 1px 线。
**主体：** `padx=16`，`pady=12`。

1. 标题 `投票历史（5 分钟内且投票未结束时可改票）` — `FAMILY 11 bold`，`pady=(0,6)`
2. 历史表格：`ttk.Treeview`，`show="headings"`，**`height=9`**，样式 `Net.Treeview`（行高 30 / 字体 10）：

| 列 id | 表头 | 宽 |
|---|---|---|
| `ts` | `时间` | 130 |
| `who` | `申请人` | 110 |
| `mac` | `物理地址` | 180 |
| `my` | `我的投票` | 100 |
| `state` | `状态` | 100 |
| `note` | `说明` | 320 |

- 行 `iid = c["req_id"]`；数据 = `(fmt_time(created_ts), name, mac_pretty, my, state, note)`
  - `my` = `{True:"同意", False:"拒绝", None:"弃权"}.get(my_choice, "未投票")`，若 `can_change` 为真则**追加 `（可改）`**
  - `state` = `{pending:"投票中", approved:"已通过", rejected:"未通过"}.get(state, state)`（**未知状态原样显示**）
  - `note` = `c["note"] or ("剩余 " + _fmt_remain(remain) if state=="pending" else "")`
- 表格填满；下方 `hbar`：`更改所选投票` 的 `_flat_btn`（左）+ 灰字 `（只有 5 分钟内、且投票还没结束的才能改）`（`FAMILY 8`、`P.sub`、`padx=8`）；`pady=(6,14)`
- 3. 标题 `工具` — `FAMILY 11 bold`，`pady=(0,6)`
- 4. 工具行：`打开数据目录`（→ `_open_data_dir`）、`检查更新`（→ `_on_manual_update`）、`设置`（→ `_open_settings`），三个 `_flat_btn` 左排，前两个右外边距 8

### 3.7 投票卡片（悬浮面板与投票页共用）`_vote_card_widget`（源码 3136–3186）

**外层：** 卡片 `tk.Frame(bg=P.panel)`，`pack(fill="x", pady=6)`。空列表时 `_render_vote_cards` 显示 `暂无待处理的投票`（`FAMILY 10`、`P.sub`、左对齐、`pady=6`）。

**逐元素：**

| 行 | 内容 | 样式 |
|---|---|---|
| 1 | `「{c['name']}」申请加入内网` | `FAMILY 11 bold`，`fg=P.text`，左；容器 `padx=14`、`pady=(10,4)` |
| 1b | 状态文字 | `pending→投票中`、`approved→已通过`、`rejected→未通过`（未知原样）；颜色：approved → `P.accent`，rejected → `P.badge`，其他 → `P.sub`；`FAMILY 9 bold`，`padx=8` |
| 1c | `（我自己）`（仅 `from_me`） | `FAMILY 9`，`fg=P.sub` |
| 2 | `物理地址 {mac_pretty} · 内网 IP {ip or '-'} · 同意 {approvals}/{voters} 人（需 {need} 票） · 弃权 {abstains} · 反对 {rejects}` | `FAMILY 9`，`fg=P.sub`，左对齐，`padx=14` |
| 3 | `规则：{desc}` | `FAMILY 8`，`fg=P.sub`，`padx=14`，`pady=(2,0)` |
| 4 | `投票剩余 {_fmt_remain(remain)}`（仅 pending） | `FAMILY 8`，`fg=P.sub`，`padx=14`，`pady=(2,6)` |
| 5 | 按钮行 `bar` | `padx=14`，`pady=(4,12)` |

**按钮行逻辑：**
- `can_vote = (state == "pending") and (not from_me) and core.in_net() and (req_id in core.net_reqs)`
- **可投票且未投过**：三个按钮，顺序 `同意` / `拒绝` / `弃权`，每个 `side=left`、右外边距 6
  - `同意`：`bg=P.accent`、`fg=#ffffff`、flat、`bd=0`；回调值 `True`
  - `拒绝`：`bg=P.panel`、`fg=P.badge`、`relief=solid`、`bd=1`；回调值 `False`
  - `弃权`：`bg=P.panel`、`fg=P.sub`、实线框；回调值 `None`
  - 全部：按下背景 `P.hover`、手型、`FAMILY 9 bold`、`padx=12`、`pady=3`
- **可投票但已投过**：
  - `我已投：{同意|拒绝|弃权}`（`FAMILY 9 bold`，`fg=P.text`，右外边距 10）
  - 若 `change_left > 0`：灰字 `（{_fmt_remain(change_left)}内可改票）`（`FAMILY 8`、`P.sub`、右外边距 8）+ 三个改票按钮 `改同意` / `改拒绝` / `改弃权`（配色同「未投过」，但 `bg=P.panel` 配 `P.accent` / `P.badge` / `P.sub` 前景）
  - 否则：灰字 `（已超过 5 分钟，不能再改票）`（`FAMILY 8`、`P.sub`）
- **不可投票**（别人发起的、已结束、或非本内网）：灰字 `c["note"] or "等待成员投票…"`，`FAMILY 9`、`fg=P.sub`、**`wraplength=560`**、`justify=left`、左对齐放置

**时间格式化 `_fmt_remain(sec)`（源码 3114–3123，必须逐字复刻）：**
```
sec = int(max(0, sec or 0))
sec >= 86400 → "{sec//86400} 天 {sec%86400//3600} 小时"
sec >= 3600  → "{sec//3600} 小时 {sec%3600//60} 分"
sec >= 60    → "{sec//60} 分 {sec%60} 秒"
否则          → "{sec} 秒"
```
（数字与中文之间**有一个空格**。）

### 3.8 群文件视图 `view_files`（源码 3584–3616 + `_render_files` 4093–4102）

**⚠️ 打包顺序陷阱：** 按钮栏 `fbar` 在代码里是**最后**创建的，但它是 `side="top"`，而 Treeview/滚动条是 `side="left"`。Tk 的 `pack` 中 `top` **优先**于 `left` 占用空间 → 最终布局是：**按钮栏横跨整个视图宽度、贴在最上方**（`padx=14`、`pady=(10,0)`），其下才是「Treeview（左，`padx=(14,0)`、`pady=10`）+ 默认滚动条（左侧紧邻，`fill=y`、`pady=10`）」。移植时按钮栏必须画在表格上方，忽略源码里的书写顺序。

| 元素 | 数值 |
|---|---|
| 按钮栏 `fbar` | `side="top"`，`fill="x"`，`padx=14`，`pady=(10,0)`，左对齐 |
| 按钮 ×3 | `打开所选文件`、`重试下载`、`打开下载目录`；样式：`P.panel` 底、`fg=P.sub`、按下 `P.hover`、`relief=solid`、`bd=1`、手型、`FAMILY 9`；各 `side=left`、右外边距 8 |
| 文件表 | `ttk.Treeview`，`show="headings"`，样式 `Files.Treeview`：**行高 34**、字体 `FAMILY 10`、背景/字段背景 `P.panel`、表头 `FAMILY 10 bold` 前景 `P.sub`、选中行 `P.selected`/`P.text`；`height` 未指定 → Tk 默认 **10 行**；`pack(side="left", fill="both", expand=True, padx=(14,0), pady=10)` |
| 滚动条 | Tk 默认竖直滚动条，`side="left", fill="y", pady=10`（**在表格右侧**） |
| 双击 | `<Double-1>` → `_on_open_selected_file()` |

**列：**

| 列 id | 表头 | 宽 |
|---|---|---|
| `name` | `文件名` | 320 |
| `size` | `大小` | 100 |
| `sender` | `发送人` | 130 |
| `status` | `状态` | 160 |

**行数据与排序（`_render_files`）：**
- 行 `iid = r["id"]`（文件 SHA-256），values = `(name, human_size(size), sender, status)`
- 排序键：`(r["status"] == "已下载", -size)` → **未下载的排前面；同组内按文件大小从大到小**。
- `human_size(n)`：循环 `B/KB/MB/GB/TB`，`n < 1024` 时输出；B 用整数 + ` B`，其余 `{n:.1f} {unit}`（**保留 1 位小数**，如 `1.5 MB`）。

### 3.9 成员视图 `view_members`（源码 3619–3657）

- 容器内一个 `ScrollFrame`：`fill=both expand=True`，`padx=14`，`pady=10`。
- 首个子控件固定为 `members_count_lbl`：`FAMILY 9`、`fg=P.sub`、左对齐、`pady=(0,6)`；文本 `本群在线成员 · {n} 人 · 点击成员头像可私聊`（n = `core.group_members(gid)` 长度，**含自己**）。
- 每个成员一张卡片：`tk.Frame(bg=P.panel)`，`pack(fill="x", pady=3)`，非自己时手型：

| 顺序 | 元素 | 数值 |
|---|---|---|
| 1 | `Avatar(36)` | `side=left`，`padx=(10,8)` |
| 2 | 信息块 | `side=left`，横向填满+伸缩，`pady=7`；上行 = `{name}`，自己追加 `（我）`，`FAMILY 10 bold`、`fg=P.text`、左对齐、横向填满；下行 = `局域网地址 {ip}`，`FAMILY 8`、`fg=P.sub`、横向填满 |
| 3 | 右侧 | 自己 → `我自己`（`FAMILY 8`、`P.sub`、`side=right`、`padx=10`）；别人 → `私聊 →`（`FAMILY 9`、`fg=P.accent`、`side=right`、`padx=10`） |
| 4 | 底部 1px 分隔线 | `tk.Frame(height=1, bg=P.border)`，`side=bottom`、`fill=x` |

- 别人的卡片：`_bind_click(card, ...)` → `open_dm(m["uid"])`（**点整张卡片**，含头像与文字）。自己的卡片不绑定点击。
- **注意：** 成员来自 `group_members()`，自己的 IP 固定为字符串 `本机`。

### 3.10 投票悬浮面板 `_show_vote_popup`（源码 3329–3391）

- 触发：鼠标进入投票导航按钮（frame/图标/文字任一）后 **延迟 200 ms**；若已有定时器则取消重排。
- 隐藏：鼠标离开投票按钮后 **延迟 450 ms**（`_schedule_hide_job`）；面板自身的 `Enter` 取消隐藏、`Leave` 重新计时。
- 面板是**无边框窗口**（`overrideredirect(True)`），并设 `-topmost`（失败忽略）。
- 外框：窗口底 `P.border`，内部 `outer` 为 `P.panel`，`padx=1, pady=1` → **视觉上是 1px 的灰边框**。
- 头部：`投票`（`FAMILY 11 bold`，`P.text`）+ `鼠标移开自动收起`（`FAMILY 8`，`P.sub`，`padx=8`）；容器 `padx=12`，`pady=(10,4)`。
- 主体 `_vote_popup_body`：`padx=12`；内容 = `core.net_pending_for_me()` 的投票卡片；为空时显示 `暂无待我投票的入网申请`（`FAMILY 10`、`P.sub`、左对齐、`pady=10`）。
- 底部按钮 `打开投票页面`：右对齐（`anchor="e"`、`padx=12`、`pady=(4,10)`），`FAMILY 9`、`fg=P.accent`、无边框、按下 `P.hover`；点击 → 先隐藏面板，再 `switch_nav("vote")`。
- **定位算法（必须复刻）：**
  ```
  btn = 投票导航按钮
  x = btn.screen_x + btn.width + 6        # 按钮右侧 6px（btn 不存在时 x = 120）
  y = btn.screen_y - 12                   # 按钮上方 12px（btn 不存在时 y = 220）
  w, h = 面板期望宽高
  if x + w > 屏幕宽 - 20: x = max(20, 屏幕宽 - w - 20)
  if y + h > 屏幕高 - 40: y = max(20, 屏幕高 - h - 60)
  ```
  即面板在按钮**右侧 +6 / 上移 12**，越界时贴边（水平留 20px，垂直下留 40、上留 20，回退时额外再上移 20）。

### 3.11 私聊窗口 `ChatWindow`（源码 4234–4367，可多开，一个 uid 一个窗口）

| 项 | 值 |
|---|---|
| 类型 | 独立顶层窗口（Tk `Toplevel`），父窗口 = 主窗口，`transient` |
| 标题 | `私聊 · {peer_name}`；有未读数时 → `私聊 · {peer_name}（{n} 条新消息）` |
| 尺寸 | **440 × 580** |
| 居中 | `x = 主窗.x + (主窗宽 - 440)//2 + 90`；`y = 主窗.y + max((主窗高 - 580)//2 - 20, 0)`；负数取 0 |
| 背景 | `P.panel` |
| 关闭 | 从 `app.dm_windows` 中移除自己后销毁 |

**内部布局：**

1. **头部高 52**（`pack_propagate(False)`，`P.panel`）：`Avatar(34)`，`padx=(12,8)`，`pady=9`；右侧块 `pady=7`：名字（`FAMILY 11 bold`，`P.text`，左对齐）+ 状态行（`FAMILY 8`，左对齐）：
   - 在线（`core.is_online(uid)`）→ 文本 `● 在线`，颜色 `P.accent`
   - 否则 → `○ 可能离线`，颜色 `P.sub`
   - 状态每次刷新时同步重设标题
2. 头部下 1px `P.border` 线
3. **消息区**：`ScrollFrame` 填满；内部 `msg_bg`（`bg=P.bg`）
4. **输入区**（`side=bottom`、`P.panel`）：顶部 1px `P.border`；`row` 内边距 `padx=10`、`pady=10`；输入框 `#f7f7f7`、`FAMILY 10`、横向填满、`ipady=7`、右外边距 8、回车发送；「发送」按钮 `P.accent`/`#ffffff`、按下 `P.accent_hv`、flat、`FAMILY 10 bold`、`width=8`、`pady=7`
5. 打开时 `entry` 获得焦点；`<FocusIn>` → `_mark_read()`（未读清零并刷新标题）

**私聊消息绘制 `append_msg(m)`（与群聊基本一致，差异已标出）：**

- `sys` 类型：`── {text} ──` 居中，`FAMILY 9`、`P.sub`、`pady=3`
- 聊天：外层 `padx=14`、`pady=4`（群聊是 16/5）
- 自己的：气泡 `bg=P.bubble_self`、`wraplength=300`、`padx=12/pady=8`、`FAMILY 10`；下方 `{HH:MM:SS}  我`（`FAMILY 8`、`P.sub`、右对齐、`pady=(2,0)`）
- 对方：上方一行 = `Avatar(30)`（`padx=(0,6)`）+ 文本 `{from_name}  {HH:MM:SS}`（**名字与时间之间两个空格**）；若 `m["relayed"]` 为真再追加 `  · 离线转存送达`（**前面两个空格**）；均为 `FAMILY 8`、`P.sub`。气泡 `bg=P.bubble_other`、`wraplength=300`、左对齐、`pack(anchor="w", padx=(0,30), pady=(2,0))`
- 每条追加后**立即**滚到底
- **私聊消息不显示自己的头像；对方消息显示头像但不可点击**

**打开/前台规则 `open_dm(uid)`：** `uid` 为空或等于自己 → 返回 None，不开窗；已有窗口 → `deiconify + lift + focus`；否则用 `core.peers[uid]["name"]` 作名字（缺失时 `用户{uid前6位}`）新建窗口。

---

## 4. 交互行为全集

### 4.0 事件泵（源码 3732–3792）

- 每 **100 ms** 执行一次 `_poll_events()`：循环 `events.get_nowait()`，按 `ev[0]` 分派（见 §5.2），队列空则退出循环，最后再 `after(100, ...)` 排下一轮。
- **所有 UI 更新都发生在这个轮询里**（核心不直接碰 UI）。移植时必须保持这一约束：核心线程只入队。

### 4.1 鼠标

| 目标 | 动作 | 行为 |
|---|---|---|
| 群卡片（含全部子控件） | 左键单击 | `select_gid(gid)` |
| 群卡片 frame | 右键单击 | 在**屏幕坐标** `(x_root, y_root)` 弹出群菜单 |
| 群菜单「退出该群组」 | 左键 | 确认框（见 §4.6）→ `core.leave_group(gid)`；若退的是当前群则清空当前群、重绘聊天与文件页；随后刷新群列表与 peers |
| 聊天区「别人消息」头像 | 左键 | `open_dm(from_uid)`（仅在消息带 `from_uid` 时） |
| 聊天区「别人消息」名字 | 左键 | 同上 |
| 成员卡片（非自己） | 左键（整卡） | `open_dm(uid)` |
| 群卡片时间/徽标/副标题 | — | ⚠️ **源码中 `set_hover` 从未被调用，因此没有鼠标悬停变色效果，也没有 `onMouseLeave` 恢复逻辑。** |
| 图标栏「投票」按钮（frame/图标/文字） | 进入 | 取消隐藏任务；取消未触发的显示任务；**200 ms 后**显示投票悬浮面板 |
| 图标栏「投票」按钮 | 离开 | 取消隐藏任务；**450 ms 后**隐藏面板 |
| 投票悬浮面板 | 进入 | 取消隐藏任务 |
| 投票悬浮面板 | 离开 | 重新排 450 ms 隐藏 |
| 任意 `ScrollFrame`（群列表/聊天/投票/成员/私聊） | 滚轮（指针在容器内） | 滚动 `int(-delta/120)` 单位（Windows 上每格 1 单位，约 1/若干内容高）；离开容器即解绑 |
| 文件表 | 双击任意行 | `_on_open_selected_file()` |
| 加入群对话框的群列表 | 双击任意行 | `_join_selected()` |
| 各 `ttk.Treeview` | 单击行 | 选中（背景 `P.selected`，前景 `P.text`）；无其他副作用 |

### 4.2 键盘

| 场景 | 按键 | 行为 |
|---|---|---|
| 群聊输入框 | `Enter` | `_on_send_chat()` |
| 私聊输入框 | `Enter` | `ChatWindow._send()` |
| 创建群对话框名称框 | `Enter` | `CreateGroupDialog._ok()` |
| 设置对话框名字框 | `Enter` | `SettingsDialog._save_name()` |
| 设置对话框 | 切换焦点到名字框 | 无自动全选等行为 |
| 其他 | — | **源码没有任何全局快捷键、没有 Ctrl+… 绑定、没有 Tab 顺序定制、没有 Esc 关闭绑定。** |

### 4.3 发送群聊消息 `_on_send_chat`（源码 4064–4077）

1. `text = 输入框内容.strip()`；空 → 直接返回（不清空输入框）。
2. 无当前群 → 信息框 `提示` / `请先选择或创建一个群组`，返回。
3. `not core.in_net()` → 信息框 `提示` / `还没有加入内网，无法发送消息。`，并 `switch_nav("net")`，返回。
4. `core.send_chat(gid, text)` —— **核心内部会自行去空格并截断到 4000 字符**，然后广播并**回推 `chat` 事件（`self=True`）**；界面**不是**自己插入消息，而是等事件回来渲染（见 §5）。
5. 清空输入框。

### 4.4 发送文件 `_on_send_file`（源码 4104–4125）

1. 无当前群 → 信息框 `提示` / `请先选择或创建一个群组`，返回。
2. 文件选择对话框（多选）：标题 `选择要发送到群的文件`。
3. 未选 → 返回。
4. 确认框：标题 `发送文件`，正文 `把所选 {n} 个文件发送到群「{gname}」？\n群内所有成员将自动下载。`；选否 → 返回。
5. 在**后台线程**里逐个 `core.send_file_to_group(Path(p), gid)`，**每个文件之间 sleep 1.0 秒**；单个失败会向 GUI 队列投 `syschat` 事件：`发送文件 {文件名} 失败：{异常}`。

### 4.5 私聊发送 `ChatWindow._send`（源码 4360–4367）

1. 空文本 → 返回。
2. 若对方**不在线**：先在消息区插一条系统消息 `对方不在线，正在寻找在线同学加密转存…`。
3. `core.send_dm_auto(uid, text)`：
   - 在线 → `send_dm()`，核心回推 `dm` 事件（`self=True`）→ 窗口里回显自己的消息。
   - 离线 → 后台尝试加密转存给随机在线同学；成功后核心投 `dm_sent`（ok=True、带 `relay_name`、`text`、`ts`），界面插两条：系统消息 `对方不在线，已加密转存给在线同学「{relay_name}」，待对方上线后自动送达` + 自己那条消息；全部失败 → 投 `dm_sent`（ok=False、`reason`），界面插系统消息 `对方不在线，且没有其他在线同学可代为转存`。
4. 清空输入框。

### 4.6 各确认/提示框（文案逐字，移植时按原生消息框实现）

| 触发 | 类型 | 标题 | 正文 |
|---|---|---|---|
| 退出群（菜单 / 顶部按钮） | 是/否 | `退出群组` | `确定退出群「{gname}」吗？\n（已下载的文件会保留在本机）` |
| 退出内网 | 是/否 | `退出内网` | `确定退出内网「{name}」吗？\n退出后你会重新扫描附近内网（可能被要求重新投票加入）。` |
| 发送文件 | 是/否 | `发送文件` | 见 §4.4 第 4 步 |
| 未入网时创建群 | 信息 | `提示` | `请先加入或建立一个内网，再创建群组。`（随后跳到内网页） |
| 未入网时加入群 | 信息 | `提示` | `请先加入或建立一个内网，再加入群组。`（随后跳到内网页） |
| 内网页「申请加入所选内网」但已入网 | 信息 | `提示` | `你已经在内网里了` |
| 内网页未选中任何内网 | 信息 | `提示` | `请先在下面选择要加入的内网（可先「重新扫描」）` |
| 建内网失败 | 错误 | `建立内网失败` | 异常字符串 |
| 申请加入失败 | 错误 | `申请失败` | 异常字符串 |
| 重命名失败 | 错误 | `重命名失败` | 异常字符串 |
| 投票异常（`net_cast_vote` 抛错） | 信息 | `投票` | 异常字符串 |
| 建立内网名称输入 | 输入框 | `建立内网` | 提示 `给内网起个名字（如：高三（2）班内网）`，初值 `{socket.gethostname()} 的内网`；取消（None）→ 什么都不做 |
| 重命名内网输入 | 输入框 | `重命名内网` | 提示 `新的内网名称：`，初值 = 当前内网名；空串 → 返回；成功则 `core.net_rename(name.strip())` |
| 未选中文件就打开 | 信息 | `提示` | `请先在群文件列表中选择一个文件` |
| 文件未下载完 | 信息 | `提示` | `该文件尚未下载到本机，正在等待自动下载完成。` |
| 打开文件失败 | 错误 | `错误` | `无法用系统默认程序打开该文件` |
| 投票历史未选中 | 信息 | `提示` | `请先在投票历史里选一条记录` |
| 投票历史选中但不可改 | 信息 | `提示` | `这条投票不能改了：只有投票未结束、且投出后 5 分钟内的才能改。` |
| 创建群名为空 | 信息 | `提示` | `请输入群组名称`（父窗口是对话框本身） |
| 创建群遇 PermissionError | 信息 | `提示` | 异常字符串，随后跳到内网页 |
| 设置里名字为空 | 信息 | `提示` | `名字不能为空`（父窗口是对话框本身） |
| 发起改名投票失败 | 错误 | `发起投票失败` | 异常字符串 |
| 改名投票未通过 | 警告 | `改名未通过` | `{reason}\n\n可重新发起投票。` |
| 加入群手动输入无群号 | 信息 | `提示` | `请输入群号（向创建者索取）` |
| 启动失败 | 错误 | `启动失败` | `程序启动失败，请查看控制台错误信息。`（无父窗口） |
| 更新程序缺失 | 警告 | `找不到更新程序` | `同目录下没有独立更新程序：\n{updater}\n\n请把 {UPDATER_PROGRAM_NAME} 与本程序放在同一个文件夹里，或直接运行「更新.bat」。` |
| 启动更新程序失败 | 警告 | `启动更新程序失败` | `{异常类型}: {异常}` |
| 已启动更新程序 | 信息 | `已启动更新程序` | `已启动独立更新程序：\n{updater}\n\n请在它弹出的窗口里完成更新（会先把你的会话内容备份到 {CUSTOMER_DATA_DIR}/，\n再把新版本取回并接着用这些会话内容）。\n\n更新完成后关闭本程序，到新版本目录双击「启动.bat」即可。` |
| 发现新版本（见 §4.10） | 是/否/取消 | `发现新版本` | 见 §4.10 |

### 4.7 导航与 chip 切换

- `switch_nav(key)`：记录 `_nav`；把这一个按钮**所有子控件**背景设 `P.selected`，其余设 `P.panel`；把 5 个右侧页面全部隐藏，再显示目标页；按需刷新：
  - `net` → `_render_net_page()`
  - `vote` → `_render_vote_page()`
  - `more` → `_render_more_page()`
  - `chat` → `_raise_view(_chip_cur)`
  - `log` → 只显示，不额外刷新
  - 未知 key → 回退到 `right_main`
- `_set_chip(key)`：记录 `_chip_cur`；3 个 chip 按 §2.5 切换样式；然后 `_raise_view(key)`。

### 4.8 选中群组 `select_gid(gid)`（源码 4023–4036）

1. `gid` 不在 `core.groups` → 直接返回。
2. `current_gid = gid`；`unread[gid] = 0`。
3. 遍历所有群卡片：`set_selected(_gid == gid)`；当前群额外 `set_badge(0)`（**即点击后立即清未读红点**）。
4. 依次 `_render_chat()`、`_render_files()`、`_render_members()`（三者都重建，即使当前不可见）。
5. `_update_group_header()` → 更新头部群名/信息，并 `_set_chip(_chip_cur)`（会再触发一次 `_raise_view`）。

### 4.9 群列表刷新 `_refresh_groups_list()`（源码 3660–3683）

1. 销毁全部旧卡片。
2. 过滤关键字：占位态下为空；否则 `search_var` 去空格转小写。
3. 遍历 `core.my_groups()`，按 §2.4 过滤，创建卡片（`pack(fill="x", padx=8, pady=2)`），`set_selected`、`set_badge(unread)`。
4. 预览：从 `chat_cache[gid]` **倒序**找第一条 `t == "chat"`，显示 `"{from}: {text}"` + 它的时间戳；找不到则保持 `点击进入群聊` 且时间留空。
5. 若 `current_gid` 已不在 `core.groups` 中 → 置空。
6. `_update_group_header()`，然后 `_raise_view(_chip_cur)`。

### 4.10 更新检查与弹窗（源码 3940–4008）

- 启动时自动 `core.check_update(manual=False)`；手动入口在「更多」页和设置对话框。
- 手动检查前置状态：`_manually_check`（防重复点击）、`_update_shown = False`（允许再次弹窗）。
- 结果事件 `update`：
  - `ok=False` / `ok=True 且 found=False` → **完全静默，无任何 UI 反馈**（这是明确的设计要求）。
  - `ok=True 且 found=True` → 若 `_update_shown` 已为真则忽略；否则置真并弹**是 / 否 / 取消**框：
    ```
    新版本 —— v{remote}

    更新内容：
    {notes}

    （当前版本 v{local}）

    「是」立即更新：启动同目录下的独立更新程序 {UPDATER_PROGRAM_NAME}，
    它会先把你的所有会话内容备份到 {CUSTOMER_DATA_DIR} 文件夹，
    再取回新版本并把你的会话内容接着用；
    「否」打开更新页面手动下载；「取消」稍后再说。
    ```
    （`notes` 为空时省掉「更新内容」那两行；`CUSTOMER_DATA_DIR = "customer_god"`，`UPDATER_PROGRAM_NAME = "neiwang_update.py"`）
    - 是 → `_start_update()`（用 `sys.executable` 启动同目录更新程序，参数 `--update`，工作目录为更新程序所在目录；`_updating` 防抖；成功后写一条日志并弹「已启动更新程序」；失败弹相应警告）
    - 否 → 若 `update_page` 非空则用系统浏览器打开
    - 取消 → **`_update_shown` 复位为 False**（下次还能再提醒）

### 4.11 内网操作（源码 3394–3450）

| 操作 | 行为 |
|---|---|
| `_on_net_scan()` | 先把 `net_apply_lbl` 置为 `正在扫描附近的内网…`；后台线程调 `core.net_scan(NET_SCAN_WINDOW=3 秒)`；**1500 ms 后**若当前在内网页则重绘该页 |
| `_on_net_create()` | 已入网则返回；弹输入框（见 §4.6）；`name.strip()`；`core.net_create(...)`；异常弹 `建立内网失败` |
| `_on_net_apply_selected()` | 已入网 → `提示/你已经在内网里了`；未选中 → `提示/请先在下面选择要加入的内网（可先「重新扫描」）`；否则 `core.net_apply_join(选中的 nid)`（nid 即 Treeview 的 `iid`）；异常弹 `申请失败` |
| `_on_net_leave()` | 未入网则返回；确认后 `core.net_leave()` |
| `_on_net_rename()` | 未入网则返回；输入新名后 `core.net_rename(name.strip())` |
| `_on_net_cancel_apply()` | 直接 `core.net_cancel_apply()`（无确认） |

### 4.12 内部状态联动（`net_changed` / `net_vote_changed` / `net_result`）

- `_on_net_changed()`：更新投票红点 → 按当前 `_nav` 重绘 `net` / `vote` / `more` 页 → `_refresh_peers()` → 若 `_nav == "chat"` 则 `_raise_view(_chip_cur)`。全过程 `try/except` 包裹，异常只打印堆栈。
- `_on_net_vote_changed()`：更新红点 → 按 `_nav` 重绘 `vote` 或 `net` 页 → 刷新投票悬浮面板内容（若开着）。
- `_on_net_result(ok, text)`：写一条日志 `[{时间}] 内网: {text}` → 系统通知（标题 `内网`，正文 `text`）→ **若 `ok` 为真则再弹信息框**（标题 `内网`）→ 调用 `_on_net_changed()`。
- `_cast_vote(req_id, val)`：调 `core.net_cast_vote(req_id, val)`；异常 → 信息框 `投票` + 异常文本并返回；成功 → 更新红点 → 按 `_nav` 重绘 `vote`/`net` 页 → 刷新悬浮面板内容。

### 4.13 文件打开与目录行为（源码 4139–4186）

- `_on_open_selected_file()`：
  1. 无选中行 → `提示/请先在群文件列表中选择一个文件`。
  2. `path = Path(rec["path"])`；若不是文件 → 尝试 `core.downloads_dir(current_gid, gname) / sanitize_name(rec["name"])`。
  3. 仍不是文件 → `提示/该文件尚未下载到本机，正在等待自动下载完成。`
  4. `open_with_system_app(path)`：Windows 用 `os.startfile(str(path))`；macOS `open`；其他 `xdg-open`。失败 → `错误/无法用系统默认程序打开该文件`。
- `_on_retry_download()`：无选中则返回；构造任务 `{fid, name, size, gid, gname, sender_uid(来自 `core.index_record(fid).src_uid`), sender_name}` 投入 `core._download_queue`。
- `_on_open_downloads()`：创建 `{DATA_DIR}/downloads` 后打开该目录（Windows `os.startfile`，其他 `webbrowser.open`）。
- `_open_data_dir()`：Windows `os.startfile(DATA_DIR)`，其他 `webbrowser.open(DATA_DIR.as_uri())`。`DATA_DIR = 程序目录/data`（可用环境变量 `NEIWANG_DATA_DIR` 覆盖）。
- `sanitize_name(s, maxlen)`：把 `\ / : * ? " < > | \r \n \t` 全替换为 `_`，去首尾空白，空则 `未命名`，再截断。
- `fmt_mac(uid)`：12 位纯十六进制 → 每 2 位插 `-` 并转大写（`AA-BB-CC-DD-EE-FF`）；`RND` + 12 位（总长 15）→ `RND-XXXX-XXXX-XXXX`；其他 → 原样或 `未知`。

---

## 5. 与 PeerCore 的全部接口

### 5.1 构造与生命周期

```python
cfg = load_config()                                  # 读写 {DATA_DIR}/config.json
self.events = queue.Queue()
self.core = PeerCore(self.cfg, self.events, name_getter=lambda: self.cfg["name"])
self.notifier = SystemNotifier(self.root)            # 传主窗口句柄

core.start()                                         # 启动 6 个后台线程：udp/tcp/presence/sweep/download/net
core.check_update(manual=False)                      # 启动即检查更新（后台线程）
...
core.stop()                                          # 退出时调用（_running=False + join 各线程 ≤1.5s）
```

- `PeerCore.__init__(cfg, events, name_getter=None, data_root=None)`；`core.uid = cfg["uid"]`（本机唯一 ID，形如 MAC 或 `RNDxxxxxxxxxxxx`）。
- `cfg` 中 GUI 直接读取的键：`name`、`uid`、`udp_port`、`tcp_port`、`update_page`、`groups`、`rename_vote_need`。
- GUI 直接读取的 core 属性（只读）：`core.groups`（`gid → {gid, gname}`）、`core.peers`（`uid → {name, ip, tcp_port, groups, ver, ts}`）、`core.discovered`（`gid → {gname, host_uid, host_name, ts}`）、`core.net_reqs`、`core.net_applying`。

### 5.2 GUI 调用的核心方法（全部）

| 方法 | 参数 | 返回 | 说明 / 调用点 |
|---|---|---|---|
| `start()` | — | `None` | 启动全部后台线程 |
| `stop()` | — | `None` | 关闭时调用 |
| `check_update(manual)` | `bool` | `None` | 启动 / 手动检查更新（后台线程） |
| `my_tcp_port()` | — | `int` | 设置对话框显示 TCP 端口 |
| `my_groups()` | — | `list[{"gid","gname"}]` | 群列表刷新、群文件模型重建 |
| `group_members(gid)` | `str` | `list[{"name","uid","ip","self"}]` | 成员页 + 头部在线人数；**含自己，自己 ip 固定为 `本机`** |
| `is_online(uid)` | `str` | `bool` | 私聊在线状态、发送前判断 |
| `in_net()` | — | `bool` | 几乎所有群功能的准入判断（决定 netgate 是否显示） |
| `send_chat(gid, text)` | `str, str` | `None` | 发送群消息；**内部 strip + 截断 4000**；未入网会抛 `PermissionError` |
| `send_dm_auto(to_uid, text)` | `str, str` | `dict`（`{ok, direct}` / `{ok, pending}` / `{ok: False, reason}`） | 私聊自动直发或离线转存 |
| `create_group(gname)` | `str` | `{"gid","gname"}` | 未入网抛 `PermissionError`；群名 `sanitize_name(..., 40)`；gid = 6 位大写十六进制 |
| `join_group(gid, gname="")` | `str, str` | `{"gid","gname"}` | gid 转大写；gname 为空则用 `discovered` 里的名字，否则 `群{gid[:6]}` |
| `leave_group(gid)` | `str` | `None` | |
| `vote_need`（property） | — | `int` | 默认 5（`RENAME_VOTE_NEED`，可被 cfg 覆盖）；改名需**超过**此票数 |
| `rename_needs_vote()` | — | `bool` | `rename_count > 0`（即已改过一次名） |
| `rename(new_name)` | `str` | `None` | 仅第一次改名可直接生效；否则抛 `PermissionError` |
| `start_rename_vote(new_name)` | `str` | `pid` | 发起改名投票；已有进行中的投票抛 `RuntimeError`；名称相同抛 `ValueError` |
| `cast_rename_vote(pid, approve)` | `str, bool` | `None` | 同一提案只记一票 |
| `net_view()` | — | `dict` | 见下方字段表 |
| `net_nearby_list()` | — | `list` | 按成员数降序、nid 升序 |
| `net_pending_for_me()` | — | `list[card]` | **待我投票**：pending + 同 nid + 非我自己 + 我在 voters 里 + 我还没投过；按创建时间倒序 |
| `net_open_votes()` | — | `list[card]` | 所有进行中投票（含我发起的、我已投的）；未入网时只返回我自己发起的那些 |
| `net_vote_history()` | — | `list[card]` | 所有请求的 card + `can_change` 字段；按创建时间倒序 |
| `net_create(name="")` | `str` | `dict(net)` | 已入网抛 `RuntimeError`；名称 `sanitize_name(..., 30)` |
| `net_leave()` | — | `None` | 退出后后台自动重扫，且**不再自动加回** |
| `net_rename(name)` | `str` | `None` | 非发起者抛 `PermissionError`；名称 `sanitize_name(..., 30)` |
| `net_scan(seconds=None)` | `float` | `list` | 阻塞式扫描（默认 3 秒），每秒广播一次 |
| `net_apply_join(nid)` | `str` | `dict(net_applying)` | 已入网抛 `RuntimeError`；找不到 nid 抛 `ValueError` |
| `net_cancel_apply()` | — | `None` | |
| `net_cast_vote(req_id, approve)` | `str, True/False/None` | `None` | 异常：找不到投票 / 已结束 / 非本内网 / 无投票权 / **超过 5 分钟不能改票** |
| `send_file_to_group(path, gid)` | `Path, str` | `None` | 未入网抛 `PermissionError`；文件不存在/群不存在抛 `ValueError`；发送前投 `syschat` |
| `files_of_group(gid)` | `str` | `list[file_view]` | 启动时重建文件模型 |
| `index_record(file_id)` | `str` | `dict|None` | 重试下载时取 `src_uid` |
| `downloads_dir(gid, gname)` | `str, str` | `Path` | 打开已下载文件时定位 |
| `_download_queue`（属性，非方法） | `queue.Queue` | — | GUI 直接 `put(task)` 触发重试下载 |
| `net_applying`（属性） | — | `dict|None` | **GUI 直接读取**：`{req_id, nid, name, voters, created_ts, deadline}` |

**`net_view()` 返回字段：**

```
未入网: {"in_net": False, "applying": {...}|None, "nearby": [ ... ]}
已入网: {
  "in_net": True, "nid", "name", "founder_mac",
  "is_founder": bool,             # founder_mac == 本机 uid
  "my_mac": 本机 uid, "my_mac_pretty": fmt_mac(uid),
  "member_count": int,
  "members": [ {mac, name, ip, tcp_port, founder, joined_ts, ...} ]  # 群主优先、再按加入时间升序
  "online_count": int,            # 在线成员数（自己也算在线）
  "created_ts": float
}
```
附近内网条目（`nearby[i]`）：`{nid, name, founder_mac, macs, member_count, ts}`。

**投票 card（`net_pending_for_me` / `net_open_votes` / `net_vote_history` 的元素）字段：**

| 字段 | 含义 |
|---|---|
| `req_id` | 请求 id |
| `name` / `mac` / `mac_pretty` / `ip` | 申请人名字 / 物理地址 / 格式化地址 / 内网 IP |
| `state` | `pending` / `approved` / `rejected` |
| `note` | 结果说明（未结束时为空） |
| `created_ts` / `deadline` / `remain` | 创建时间 / 截止时间 / **剩余秒数（≥0）** |
| `from_me` | 是否我自己发起的申请 |
| `voted` / `my_choice` / `my_ts` | 我是否投过 / 我的选择（`True`/`False`/`None`）/ 我投票的时间 |
| `change_left` | 还能改票的剩余秒数（`NET_VOTE_CHANGE=300` 减去已过时间，未投时为 0） |
| `voters` / `need` / `desc` | 有投票权的人数 / 需要的同意票数 / **规则说明文本** |
| `approvals` / `rejects` / `abstains` / `approvers` / `rejecters` | 同意数 / 反对数 / 弃权数 / 同意者列表 / 反对者列表 |
| `can_change`（仅历史） | `voted and state=="pending" and change_left > 0` |

**入网投票规则（`net_vote_need(voters)`，源码 183–199）：**
- `voters <= 5` → 需要 `0` 票（免投票自动通过），说明 `内网 {n} 人（不超过 5 人），免投票自动通过`
- `6 <= voters <= 7` → 需要 `voters` 票（全票），说明 `内网 {n} 人（6–7 人），需全票通过（{n} 票）`
- `voters >= 8` → 需要 `voters // 3 + 1` 票，说明 `内网 {n} 人（8 人及以上），需超过 1/3 同意即 {need} 票`

### 5.3 核心推给 GUI 的事件（逐条）

事件统一是 `events` 队列里的 **元组**：`(kind, payload1, payload2, ...)`。GUI 每 100 ms 取空。

| kind | 载荷 | 触发时机 | GUI 处理 |
|---|---|---|---|
| `syslog` | `str` | `start()` 完成 | 日志页追加 `[{当前时间}] 系统: {文本}` |
| `log` | `str`（**已含时间前缀的整行**） | 公共日志（本机广播 / 收到别人的） | 日志页原样追加 |
| `chat` | `dict{gid, gname, from_name, from_uid, text, ts, self}` | 自己发（`self=True`）/ 收到群消息 | 见 §5.4 |
| `syschat` | `gid: str, text: str` | 文件指纹计算、自动下载开始、下载完成、发送失败等 | 写入 `chat_cache[gid]` 作 `{"t":"sys",...}`；若是当前群且在聊天页则增量渲染 |
| `peers_changed` | 无 | presence 变化 / 定时清扫 | `_refresh_peers()` |
| `groups_changed` | 无 | 创建 / 加入 / 退出群 | `_refresh_groups_list()` |
| `name_changed` | `new_name: str` | 改名成功（首次直接改或投票通过） | 更新图标栏名字（截前 4 字）+ 重画我的头像 |
| `file_new` | `dict{file:{id,name,size,src_uid,src_name,ts,groups,has_local}, gid, from_self}` | 自己发送文件 / 收到 `file_pub` | 见 §5.5 |
| `file_status` | `dict{id, status, progress?, path?, note?}` | 下载中 / 完成 / 失败 | 见 §5.5 |
| `notify` | `title: str, msg: str` | 文件下载完成（标题 `文件下载完成`） | `notifier.notify(title, msg)` |
| `update` | `dict`（见下） | 更新检查结束 | `_on_update_result()`；**只有 `ok=True 且 found=True` 才弹窗** |
| `rename_request` | `dict{pid, from_uid, from_name, old, new}` | 收到别人的改名提案 | 若 `from_uid` 是自己 → 忽略；否则弹 `RenameVoteDialog` |
| `rename_vote_progress` | `pid: str, approve: int, reject: int` | 自己发起的投票收到票 / 刚发起（0,0） | 转交当前设置对话框 `on_vote_progress(pid, approve, reject)` |
| `rename_vote_done` | `pid: str, ok: bool, reason: str, new_name: str` | 改名投票结束（超时或达标） | 转交 `on_vote_done(pid, ok, reason, new)` |
| `dm` | `dict{to_uid, from_uid, from_name, text, ts, self}` | 私聊收 / 发 | 见 §5.6 |
| `dm_sent` | `dict{to_uid, ok, direct, relay_name?, text?, ts?, reason?}` | 离线转存成功 / 失败 | 见 §4.5 |
| `net_changed` | 无 | 内网归属、成员、登记表变化 | `_on_net_changed()` |
| `net_vote_changed` | 无 | 入网投票本身变化（含申请重播） | `_on_net_vote_changed()` |
| `net_status` | `str` | 扫描中、已提交申请、已入网、已退出等状态文案 | 写入 `net_status_lbl`（**截断 80 字符**）+ 追加日志 `[{当前时间}] 内网: {文本}` |
| `net_nearby_changed` | 无 | 附近内网列表变化 / 扫描结束 | 仅当 `_nav == "net"` 时 `_render_net_page()` |
| `net_result` | `ok: bool, text: str` | 入网被批准 / 被拒 / 超时 | `_on_net_result(ok, text)`：日志 + 系统通知 + （`ok` 时）信息框 + `_on_net_changed()` |

**`update` 事件载荷：**
- `{"ok": False, "reason": "no-url"}` — 没有配置更新地址
- `{"ok": False, "reason": "offline"}` — 未联网 / 请求失败
- `{"ok": True, "found": False}` — 版本号相同或更旧
- `{"ok": True, "found": True, "remote", "notes", "local", "page", "repo", "package"}` — 发现新版本

### 5.4 `chat` 事件处理 `_on_ev_chat`（源码 3805–3822）

1. 追加到 `chat_cache[gid]`：`{"t":"chat", "from": from_name, "text", "ts", "self": bool(self), "from_uid"}`。
2. **缓存上限 2000 条**：超出则只保留最后 2000 条。
3. 若群卡片存在 → 更新副标题 `"{from_name}: {text}"` 与时间。
4. 若 `gid == current_gid` **且** `_nav == "chat"` → `_render_chat(append=True)`（增量渲染 + 滚动到底）。
5. **非自己**的消息，且（不是当前群 **或** 不在聊天页）→ `unread[gid] += 1`，并更新卡片徽标。

### 5.5 文件事件处理（源码 3846–3873）

**`file_new`：** 若 `file.id` 不在 `file_model[gid]` 中，插入一条：
```
{"id", "name", "size", "sender": src_name,
 "status": "已存在" if file.has_local else "等待下载",
 "progress": 1.0 if has_local else 0.0, "path": ""}
```
若 `gid == current_gid` **且** 当前 chip 是 `files` → 重绘文件表。

**`file_status`：** 遍历所有群的 `file_model` 找到该 `id` 的行：
- `status == "downloading"` → `下载中 {int(progress*100)}%`
- `status == "done"` → `已下载`，`path = st["path"]`，`progress = 1.0`，并 **`_rebuild_file_model_from_index()`**
- `status == "error"` → `失败：{note}`
- 若命中当前群且 chip 是 `files` → 重绘

**`_rebuild_file_model_from_index()`：** 遍历 `my_groups()`，对 `files_of_group(gid)` 里每个文件用 `setdefault` 补齐（不覆盖已有行的进度/路径），状态 `已下载`/`等待下载`，随后若当前 chip 是 `files` 则重绘。

### 5.6 `dm` 事件处理 `_on_ev_dm`（源码 3905–3924）

1. `uid = m["to_uid"]`（自己发的）或 `m["from_uid"]`（收到的）。
2. 取窗口；自己发但窗口不存在 → 直接返回；收到消息而窗口不存在 → `open_dm(uid)` 新建；仍失败则返回。
3. `w.append_msg(m)`。
4. **非自己**且窗口**未聚焦** → `w.unread += 1`、刷新标题、系统通知（标题 `私聊消息`，正文 `「{from_name}」：{text 前 60 字}`）。
5. 私聊消息**不进公共日志、不影响群未读**。

---

## 6. 各对话框规格

所有对话框除特殊说明外：`transient(父窗口)`、`grab_set()`（模态）、`resizable(False, False)`、背景 `P.panel`、内容区 `padx/pady` 见下、无标题栏图标设置、**无 Esc 关闭、无回车默认按钮（除列出的输入框）**。

### 6.1 创建群组 `CreateGroupDialog`（源码 4373–4433）

| 项 | 值 |
|---|---|
| 标题 | `创建群组` |
| 尺寸 | **360 × 210**，不可缩放 |
| 居中 | `x = 主窗.x + (主窗宽-360)//2`；`y = 主窗.y + (主窗高-210)//3`（**除以 3**），负值取 0 |
| 内容区 | `padx=22`，`pady=20` |

控件顺序（自上而下，全部左对齐）：

1. `创建群组` — `FAMILY 14 bold`，`fg=P.text`
2. `群组名称（如：高三（2）班）` — `FAMILY 9`，`fg=P.sub`，`pady=(10,4)`
3. 输入框：`#f7f7f7`、`FAMILY 11`、无边框、`ipady=7`、横向填满、**打开即聚焦**、**回车提交**
4. `创建后自动生成群号，其他同学加入群组时输入该群号即可` — `FAMILY 8`、`P.sub`、`wraplength=310`、左对齐、`pady=(6,14)`
5. 按钮行（右对齐）：
   - `创建`（右起第一个，左外边距 10）：`P.accent`/`#ffffff`、按下 `P.accent_hv`、flat、`bd=0`、`FAMILY 10 bold`、`width=10`、`pady=5`
   - `取消`（更靠右）：`P.panel`/`P.sub`、按下 `P.hover`、flat、`FAMILY 10`

**`_ok()` 校验与返回值：**
1. `name = 去空格`；空 → 信息框 `提示` / `请输入群组名称`（父 = 本对话框），**不关窗**。
2. `app.core.create_group(name)`；**捕获 `PermissionError`** → 信息框 `提示` + 异常文本，`app.switch_nav("net")`，**不关窗**。
3. 成功 → `app._refresh_groups_list()` → `app.select_gid(g["gid"])` → 关闭窗口。
（其他异常不捕获，会冒泡到 Tk 控制台。）

### 6.2 加入群组 `JoinDialog`（源码 4630–4744）

| 项 | 值 |
|---|---|
| 标题 | `加入群组` |
| 尺寸 | **560 × 440**，不可缩放 |
| 居中 | `x = 父窗.x + (父窗宽-560)//2`；`y = 父窗.y + (父窗高-440)//3` |
| 内容区 | `padx=20`，`pady=16` |

布局：

1. `加入群组` — `FAMILY 14 bold`，`P.text`，左对齐
2. `从局域网发现的群组（自动扫描，双击加入）` — `FAMILY 9`、`P.sub`、左对齐、`pady=(4,8)`
3. **发现表**：`ttk.Treeview`，`show="headings"`，**`height=8`**，样式 `Join.Treeview`（行高 **30**、字体 `FAMILY 10`、背景 `P.panel`、表头 `FAMILY 10 bold` 前景 `P.sub`、选中行 `P.selected`/`P.text`），横向纵向填满，`pady=(0,8)`
   - 列：`群名`(150) / `群号`(90) / `群主电脑`(130) / `在线人数`(80)，均左对齐
   - 行 `iid = gid`；values = `(gname, gid, host_name, str(在线人数))`；按 `gname` 排序
   - 在线人数算法：`core.peers` 中 `now - ts <= 30` 且该 peer 的 `groups` 里含此 gid 的数量
   - **双击行 → `_join_selected()`**
4. 按钮行（左排）：
   - `加入所选群组`：`P.accent`/`#ffffff`、按下 `P.accent_hv`、flat、`FAMILY 10 bold`、`padx=14`、`pady=4`
   - `刷新`：`P.panel`/`P.sub`、按下 `P.hover`、flat、`FAMILY 10`、`padx=10`、左外边距 8
5. 1px `P.border` 分隔线，`pady=10`
6. `或者手动输入群号加入：` — `FAMILY 9`、`P.sub`、左对齐
7. 手动行：`群号`(FAMILY 10) + 输入框(`width=12`、`#f7f7f7`、`FAMILY 10`、`ipady=4`、左右外边距 6) + `群名（可留空）`(FAMILY 10) + 输入框(`width=16`、同上) + `加入`按钮(`P.accent`/`#ffffff`、flat、`FAMILY 10 bold`、`padx=12`、`pady=4`、左外边距 4)

**自动刷新：** 打开时立即 `_refresh()`，并每 **3000 ms** 自动刷新一次（窗口销毁后停止）。

**`_join_selected()`：** 未选中 → 静默返回；选中 → `core.join_group(gid, 表里该行的群名)` → `app._refresh_groups_list()` → `app.select_gid(gid)` → 关窗。

**`_join_manual()`：** `gid` 去空格为空 → 信息框 `提示`/`请输入群号（向创建者索取）`；否则 `core.join_group(gid, gname)` → 刷新列表 → `select_gid(core.groups[gid.upper()]["gid"])` → 关窗。
⚠️ `join_group` 会把 gid 转大写，所以界面用 `gid.upper()` 反查；若群号非法（例如非十六进制）核心仍会接受并建组。

### 6.3 改名投票 `RenameVoteDialog`（源码 4439–4489）

| 项 | 值 |
|---|---|
| 触发 | 收到 `rename_request` 且提案人不是自己 |
| 标题 | `改名投票` |
| 尺寸 | **未设置固定几何** → 由内容自动布局决定；**居中按 400 × 210 计算** |
| 居中 | `x = 父窗.x + 父窗宽 - 400 - 40`（**右下角区域**）；`y = 父窗.y + 父窗高 - 210 - 60`；负值取 0 |
| 其他 | 不可缩放、`transient`、尝试置顶（`-topmost`）、尝试 `grab_set()`（失败忽略）、`lift()`、`focus_set()` |
| 内容区 | `padx=22`，`pady=18` |
| **关闭拦截** | `WM_DELETE_WINDOW` 绑定为**空操作** —— 用户**必须投票**，不能直接关掉 |

内容（自上而下）：

1. `「{from_name}」申请改名` — `FAMILY 12 bold`、`P.text`
2. `从「{old}」改为「{new}」` — `FAMILY 11`、`P.text`、`pady=(8,2)`
3. `按约定：首次改名自由，之后需超过 {need} 名用户同意。\n你同意这次改名吗？` — `FAMILY 9`、`P.sub`、左对齐、`wraplength=320`、`pady=(4,14)`；`need = core.vote_need`
4. 按钮行（右对齐）：
   - `拒绝`（靠右、左外边距 8）：`P.panel`/`P.sub`、按下 `P.hover`、`relief=solid`、`bd=1`、`FAMILY 10`、`width=10`、`pady=4`
   - `同意`（在它左侧）：`P.accent`/`#ffffff`、按下 `P.accent_hv`、flat、`FAMILY 10 bold`、`width=10`、`pady=4`

**返回值：** `_vote(approve)` → `core.cast_rename_vote(pid, approve)` → 关窗。**没有取消/弃权选项**，点任一按钮即投票并关闭。

### 6.4 设置 `SettingsDialog`（源码 4495–4624）

| 项 | 值 |
|---|---|
| 标题 | `设置` |
| 尺寸 | **380 × 350**，不可缩放 |
| 居中 | `x = 父窗.x + (父窗宽-380)//2`；`y = 父窗.y + (父窗高-350)//3` |
| 内容区 | `padx=22`，`pady=18` |
| 生命周期 | 构造时把自己登记为 `app._settings_dlg`；`destroy()` 时若登记的是自己则清空（改名投票进度/结果只发给当前打开的设置框） |

内容（自上而下）：

1. `设置` — `FAMILY 14 bold`、`pady=(0,12)`
2. `我的名字（其他电脑看到的名字）` — `FAMILY 9`、`P.sub`、`pady=(4,2)`
3. 名字输入框：初值 `cfg["name"]`、`#f7f7f7`、`FAMILY 11`、无边框、`ipady=6`、横向填满、**回车 = 保存**
4. 提示语（`tip_lbl`，`FAMILY 8`、`P.sub`、`wraplength=330`、左对齐、`pady=(4,0)`）：
   - `core.rename_needs_vote()` 为真 → `提示：已改过一次名字，再次改名需超过 {core.vote_need} 名用户投票同意后才能生效。`
   - 否则 → `提示：第一次改名可以直接保存；之后的修改需要同学们投票。`
5. 按钮行（`pady=(12,4)`，左排）：
   - `保存名字`：`P.accent`/`#ffffff`、按下 `P.accent_hv`、flat、`FAMILY 10 bold`、`padx=14`、`pady=5`
   - `saved_lbl`（状态文本，`FAMILY 9`、`fg=P.accent`、左外边距 8）：初始为空，用于显示 `✓ …` 或投票进度
6. 1px 分隔线，`pady=12`
7. 两个**整行左对齐的扁平按钮**（`pack(fill="x", pady=2)`，`bg=P.panel`、`fg=P.text`、按下 `P.hover`、flat、`FAMILY 10`）：
   - `检查更新（无网络 / 版本相同则静默，仅发现新版本才提醒）` → `app._on_manual_update`
   - `打开数据目录` → `app._open_data_dir`
8. 1px 分隔线，`pady=10`
9. 版本信息（`FAMILY 8`、`P.sub`、左对齐）：
   `{APP_NAME} v{APP_VERSION} · 局域网端口 UDP {udp_port} / TCP {core.my_tcp_port()}`

**`_save_name()` 完整流程：**
1. `_voting_new is not None`（投票进行中）→ 直接返回。
2. `new = 去空格`；空 → 信息框 `提示`/`名字不能为空`（父 = 本框），返回。
3. `new == cfg["name"]` → `saved_lbl` 显示 `✓ 名字未改变`，**1200 ms 后清空**，返回。
4. `not core.rename_needs_vote()`（第一次改名）→ `core.rename(new)` → `_apply_ui(new)`（更新图标栏名字 + 重画头像 + 刷新群头部）→ `saved_lbl = ✓ 已保存（第一次改名）`，**1600 ms 后清空**。
5. 否则发起投票：`_voting_new = new`、**禁用** `保存名字` 按钮、`saved_lbl = 已发起投票：需超过 {core.vote_need} 名用户同意…`，然后 `core.start_rename_vote(new)`；抛异常 → 复位 `_voting_new`、重新启用按钮、清空 `saved_lbl`、错误框 `发起投票失败`。

**投票进行中的事件回调：**
- `on_vote_progress(pid, approve, reject)`：`_voting_new` 为空则忽略；否则 `saved_lbl = 投票中… 同意 {approve} 票（需超过 {core.vote_need} 票）`
- `on_vote_done(pid, ok, reason, new)`：`_voting_new` 为空则忽略；复位 `_voting_new`、启用按钮；`ok` 真 → `_apply_ui(new)` + `✓ 投票通过，名字已更新`（**2000 ms 后清空**）；否则清空 `saved_lbl` + 警告框 `改名未通过` / `{reason}\n\n可重新发起投票。`

### 6.5 更改投票（内联在 `_on_change_vote_selected`，源码 3251–3292）

| 项 | 值 |
|---|---|
| 触发 | 更多页「更改所选投票」按钮 |
| 标题 | `更改投票` |
| 尺寸 | 未固定（内容自适应），不可缩放 |
| 居中 | `x = 父窗.x + (父窗宽 - 对话框期望宽)//2`；`y = 父窗.y + (父窗高 - 对话框期望高)//3`；负值取 0 |
| 其他 | `transient` + `grab_set` |
| 内容区 | `padx=20`，`pady=16` |

前置校验：
1. 历史表无选中 → 信息框 `提示`/`请先在投票历史里选一条记录`，返回。
2. 选中行在 `net_vote_history()` 里找不到对应卡片 → 静默返回。
3. `can_change` 为假 → 信息框 `提示`/`这条投票不能改了：只有投票未结束、且投出后 5 分钟内的才能改。`，返回。

内容：
1. `「{name}」入网申请` — `FAMILY 11 bold`、`P.text`
2. `物理地址 {mac_pretty} · 当前我的投票：{同意|拒绝|弃权|未投票}（还剩 {_fmt_remain(change_left)} 可改）` — `FAMILY 9`、`P.sub`、`pady=(6,12)`
3. 按钮行（左排，各右外边距 8）：
   - `同意`：`P.accent`/`#ffffff`、按下 `P.hover`、flat、`bd=1`、`FAMILY 10 bold`、`padx=14`、`pady=4` → 值 `True`
   - `拒绝`：`P.panel`/`P.badge`、`relief=solid`、`bd=1`、其余同上 → 值 `False`
   - `弃权`：`P.panel`/`P.sub`、`relief=solid`、`bd=1`、其余同上 → 值 `None`
   - `取消`：`P.panel`/`P.sub`、flat、`bd=0`、`FAMILY 10`、左右外边距 8 → 直接关窗
4. 三个投票按钮的返回值：先 `_cast_vote(req_id, val)` 再关窗。
   ⚠️ **注意顺序**：源码是 `(self._cast_vote(...), dlg.destroy())`，`_cast_vote` 先执行且会重绘「更多」页 → 依赖历史表重建；移植时先提交再关窗即可。

---

## 7. 窗口通知（SystemNotifier）与文件打开

### 7.1 `SystemNotifier`（源码 370–453）

构造：`SystemNotifier(root_widget)`，保存父窗口引用用于取句柄。

`notify(title, msg)`：
1. Windows → 尝试 `_win_notify(title, msg)`（**Shell_NotifyIconW 气泡**）；成功则返回。
2. 失败或非 Windows → 退回 `_tk_popup(title, msg)`（Tk 顶层小窗）。

**Windows 实现细节（移植时可用 `Shell_NotifyIcon` 或直接换 `NOTIFYICONDATA` 的现代接口）：**

| 项 | 值 |
|---|---|
| 句柄 `hWnd` | 父窗口的 `winfo_id()`（取不到时 0） |
| 图标 id `uID` | **777** |
| 回调消息 | **`WM_APP + 50`**（`0x0400 + 50`） |
| 首次添加标志 `uFlags` | `MESSAGE(1) | ICON(2) | TIP(4)` |
| 图标 `hIcon` | `LoadIconW(0, IDI_APPLICATION = 32512)` |
| 托盘提示 `szTip` | `APP_NAME[:127]` → `内部网` |
| 添加命令 | `Shell_NotifyIconW(NIM_ADD = 0, &nid)` |
| 后续更新标志 | `TIP(4) | INFO(0x10)` |
| `szInfoTitle` | `str(title)[:63]` |
| `szInfo` | `str(msg)[:254]` |
| `dwInfoFlags` | `NIIF_INFO = 1` |
| 更新命令 | `Shell_NotifyIconW(NIM_MODIFY = 1, &nid)` |
| 首次添加失败 | 返回 False → 走 Tk 弹窗兜底 |
| 结构体顺序 | `cbSize, hWnd, uID, uFlags, uCallbackMessage, hIcon, szTip[128], dwState, dwStateMask, szInfo[256], uTimeoutOrVersion, szInfoTitle[64], dwInfoFlags, guidItem, hBalloonIcon` |

**非 Windows 兜底弹窗：** 一个置顶 `Toplevel`，标题 = title，位置 = 父窗左上角 **+60/+60**，正文 `Label`（`wraplength=340`、`justify=left`、`padx=14`、`pady=12`），**6000 ms 后自动销毁**。

**触发系统通知的三处：**
1. 文件下载完成（`notify` 事件）：标题 `文件下载完成`，正文 `「{gname}」群「{sender_name}」的「{name}」下载完毕`
2. 私聊收到消息且窗口未聚焦：标题 `私聊消息`，正文 `「{from_name}」：{text 前 60 字}`
3. `net_result`：标题 `内网`，正文 = 结果文本

### 7.2 文件打开行为汇总

| 动作 | 实现 |
|---|---|
| 打开群文件 | 优先用模型里记录的 `path`；无效则退回 `{DATA_DIR}/downloads/{群目录}/{sanitize_name(文件名)}`；都没有 → 提示等待下载；然后用系统默认程序打开：Windows `os.startfile`、macOS `open`、其他 `xdg-open`；失败弹错误框 |
| 打开下载目录 | 先 `mkdir -p {DATA_DIR}/downloads`，再打开该目录 |
| 打开数据目录 | Windows `os.startfile(DATA_DIR)`，其他 `webbrowser.open(DATA_DIR.as_uri())` |
| 启动更新程序 | `subprocess.Popen([sys.executable or "python", "{同目录}/neiwang_update.py", "--update"], cwd=更新程序所在目录)` |

---

## 8. 移植风险清单（按风险从高到低）

1. **滚动容器与「永远等宽的 inner」语义。** `ScrollFrame` 的 inner 宽度必须始终等于可视宽度（内容不横向滚动），且滚轮是**指针悬停才绑定**、单位滚动换算为 `int(-delta/120)`。Win32 侧若用 `WS_VSCROLL` 子窗口，很容易出现「内容宽度不跟随」「滚轮作用到错误控件」「子控件滚动闪烁」三类问题；建议自己画滚动条（8px 宽）+ 手动 `SetWindowPos` 平移内容，或整体改 `WS_EX_COMPOSITED` 双缓冲。
2. **动态重建控件的状态保持。** 群列表、聊天消息、投票页、成员页、文件表都会「全量销毁 + 重建」，而聊天是**增量追加**（`_rendered_n` 游标）。Win32 没有一个天然等价的「销毁再建」模型：必须显式实现「复用的行控件池 + 重排 + 失效重绘」，尤其注意「切群时消息区必须清空且计数归零」「切回聊天页时要延迟 60 ms 再滚到底」「未读徽标随选中立即清零」这些隐含状态。
3. **事件与 UI 线程的边界 + 定时器语义。** 核心线程只 `put` 元组事件，UI 每 100 ms 批量取空；同时存在大量精确定时的 UI 行为：投票面板 **200 ms 显示 / 450 ms 隐藏（进出都会重置）**、`scroll_bottom_later` 60 ms、扫描后 1500 ms 重绘、`saved_lbl` 1200/1600/2000 ms 自动清空、通知 6000 ms 自动关闭。这些定时器在 Tk 里是独立任务，Win32 侧必须用 `SetTimer`/`CreateTimerQueueTimer` 并处理「重复排程要取消旧的」「窗口已销毁要忽略」。此外还有三处**容易漏掉的模态性**：改名投票对话框拦截关闭（必须投票）、投票面板的 `overrideredirect + topmost`、设置对话框与改名投票事件的路由（`app._settings_dlg`）。
4. **本地化排版细节。** `wraplength`（气泡 430 / 私聊 300 / 投票卡片 560 / 说明 760）、中文标点的全角宽度、`padx/pady` 内边距与 `ipady` 内边距的区别、Emoji（`Segoe UI Emoji`）的彩色字形（GDI+ 需要 `DrawTextW` + `ETO_` 系列或 DirectWrite 才好看）、`── … ──` 系统消息样式，都会让像素结果与 Tk 有肉眼可见差异。
5. **颜色/字号的隐含不一致。** 搜索框用 `P.entry_bg (#f3f3f3)`，其他所有输入框硬编码 `#f7f7f7`（与窗口底色相同，视觉上「无框」）；消息区底色是 `#f7f7f7` 而非白色（源码注释有误导）；选中态只刷导航按钮的**子控件**背景、群卡片的**悬停态从未被调用**。复刻时应**照实现、不照注释/意图**，否则视觉会比原版多出高亮或底色。

---

## 附：源码行范围对照表

| 内容 | 行范围 |
|---|---|
| `FAMILY` / `EMOJI` 常量 | 2486–2487 |
| `class P` 调色板 | 2490–2505 |
| `AVATAR_COLORS` / `avatar_color()` | 2508–2513 |
| `class Avatar` | 2516–2526 |
| `class ScrollFrame` | 2529–2558 |
| `_bind_click()` | 2561–2565 |
| `class GroupCard` | 2568–2631 |
| `class App.__init__` | 2634–2669 |
| `_build_ui()`（主窗口骨架） | 2672–2843 |
| `_build_net_pages()`（内网/投票/更多） | 2846–2966 |
| `_flat_btn()` / `_apply_win11_rounding()` | 2968–2983 |
| 导航/chip/视图切换 | 2986–3040 |
| netgate / 内网页渲染 | 3043–3111 |
| `_fmt_remain` / 投票卡片 | 3114–3205 |
| 投票页 / 更多页 / 更改投票对话框 | 3208–3292 |
| 红点与投票悬浮面板 | 3295–3391 |
| 内网操作与内网事件 | 3394–3484 |
| 空状态 / 聊天视图 / 消息条 | 3487–3581 |
| 群文件视图 / 成员视图 | 3584–3657 |
| 群列表刷新 / 搜索 / 群菜单 | 3660–3729 |
| 事件泵 `_poll_events` | 3732–3802 |
| 群消息 / 系统消息 / 渲染 | 3805–3843 |
| 文件事件 | 3846–3873 |
| 改名投票入口 / 私聊入口与事件 | 3876–3938 |
| 更新检查与启动 | 3940–4008 |
| 群组操作 / 选中群 / 发送消息 | 4011–4077 |
| 文件模型与文件操作 | 4080–4186 |
| 日志追加 / 清空 | 4189–4201 |
| peers 刷新 / 关闭 | 4204–4228 |
| `class ChatWindow` | 4234–4367 |
| `class CreateGroupDialog` | 4373–4433 |
| `class RenameVoteDialog` | 4439–4489 |
| `class SettingsDialog` | 4495–4624 |
| `class JoinDialog` | 4630–4744 |
| `main()` | 4748–4760 |
