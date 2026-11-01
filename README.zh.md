<p align="center">
  <img src="packaging/icon/monogram.png" alt="SP" width="160"><br>
  <img src="docs/images/spartanport-logo.png" alt="SpartanPort" width="600">
</p>

# SpartanPort —— ARM64 Android 移植版

<div align="center">
  <a href="https://trendshift.io/repositories/299469?utm_source=trendshift-badge&amp;utm_medium=badge&amp;utm_campaign=badge-trendshift-299469" target="_blank" rel="noopener noreferrer"><img src="https://trendshift.io/api/badge/trendshift/repositories/299469/daily?language=C%2B%2B" alt="FelipePrado0%2FSpartanPort | Trendshift" width="250" height="55"/></a>
</div>

**English** | [Português (Brasil)](https://github.com/FelipePrado0/SpartanPort/blob/main/README.pt-BR.md) | [Español](https://github.com/FelipePrado0/SpartanPort/blob/main/README.es.md) | **简体中文（本文件）**

> **本仓库是上游 SpartanPort 的 ARM64 Android 移植工作分支，不是上游发布页。**
> 上游只发布 Windows 10/11 与 x86-64 Linux（实验性 AppImage）；本分支把同一套端口
> 交叉编译成 **aarch64 Linux 原生程序**，跑在 Android 平板/手机上（宿主为 **nixport**）。
> 下面第 0 章列出与上游的全部差异，第 1 章之后是上游 README 的中文版。

---

## 目录

- [0. 本仓库与上游的差异](#0-本仓库与上游的差异)
  - [一句话差异](#一句话差异)
  - [差异对照表](#差异对照表)
  - [我们具体改了什么](#我们具体改了什么)
  - [当前状态与已知限制](#当前状态与已知限制)
  - [分支说明](#分支说明)
- [1. 下载与游玩](#1-下载与游玩)
  - [1.1 ARM64 Android（本分支，主线）](#11-arm64-android本分支主线)
  - [1.2 Windows 10/11（上游）](#12-windows-1011上游)
  - [1.3 Linux x86-64 AppImage（上游，实验性）](#13-linux-x86-64-appimage上游实验性)
- [2. 能跑什么](#2-能跑什么)
- [3. 功能可用性](#3-功能可用性)
- [4. 设置](#4-设置)
- [5. 键盘操作](#5-键盘操作)
- [6. 导入 shadPS4 存档](#6-导入-shadps4-存档)
- [7. 第三方叠加层（Windows）](#7-第三方叠加层windows)
- [8. 排错](#8-排错)
- [9. 性能与兼容性](#9-性能与兼容性)
- [10. 从源码构建](#10-从源码构建)
  - [10.1 ARM64 Android（本分支）](#101-arm64-android本分支)
  - [10.2 Windows（上游）](#102-windows上游)
  - [10.3 Linux x86-64（上游）](#103-linux-x86-64上游)
- [11. 测试](#11-测试)
- [12. 工作原理](#12-工作原理)
- [13. 致谢](#13-致谢)
- [14. 许可](#14-许可)

---

## 0. 本仓库与上游的差异

### 一句话差异

上游是 **PC 原生端口**（guest x86-64 在 PC 处理器上直接跑，图形经 shadPS4 转 Vulkan）；
本分支是 **ARM64 Android 端口**：手机上没有 x86-64 处理器，游戏的 guest 代码改由
**FEXCore** 执行，图形栈仍是 shadPS4 → Vulkan（跑 Android 的 Vulkan 驱动），
并按 nixport 的 **Port 分发包**契约交付。

### 差异对照表

| 维度 | 上游 SpartanPort | 本分支（ARM64 Android） |
|---|---|---|
| 目标平台 | Windows 10/11、x86-64 Linux（实验性） | aarch64 Android（宿主 nixport） |
| guest CPU | x86-64 指令**原生**跑在 PC 的 CPU 上 | x86-64 指令由 **FEXCore**（JIT + 优化器）执行 |
| 图形 | shadPS4 → Vulkan（桌面驱动） | 同一套 shadPS4 → Vulkan（Android 驱动，如 Turnip） |
| 交付形态 | GitHub Release 的 zip / AppImage | **Port 分发包** `.zip`（`port.json` + 恰好一个 `.AppImage` + 可选 `virtualkeys.json`） |
| 屏幕操作 | 手柄 / 键盘（桌面窗口） | 宿主提供的**虚拟按键布局**（屏上控件） |
| 安装 | 解压即用 | 宿主导入 Port 包 → 落到 `files/imports/<id>/` |
| 构建 | `bash build.sh`（原生） | `bash packaging/arm-linux/build.sh`（交叉编译 + 组包） |
| 上游 `build.sh` | — | **不改**（保持上游原样，ARM64 走独立脚本） |

### 我们具体改了什么

**1）新增 ARM64 构建与打包线 `packaging/arm-linux/`**

与上游 `packaging/linux/`（x86-64 AppImage）同构，差异只有三点：

1. 工具链 = `clang --target=aarch64-linux-gnu` + Debian arm64 sysroot（`fetch-sysroot.py` 自动拉取，SHA256 锁定）；
2. 跳过 x86 宿主专属源（`isa_emulate` / `cpu_check` / `isa_rewrite`——guest x86 代码交给 FEXCore）；
3. probe 链接时**烤入 nixport rootfs 的 ELF 解释器**（Android 沙箱里没有 `/lib`）。

打包出来的程序树沿用上游 packaged 布局（`bin/spartanport-probe`、`bin/gpu/`、`run.py`、`scripts/`），
另外加 nixport 契约要求的程序入口 `entry.sh`（包契约本身是包根的 `port.json`）。依赖库策略与上游同源：打包宿主没有的库
（SDL3、ffmpeg、libgportgpu/cpu…），排除 rootfs 标配（glibc、Vulkan loader、X11、PulseAudio、fontconfig…）。

**2）运行时适配（Android 侧）**

- 游戏目录与存档落在 Android 外部存储（`/storage/emulated/0/...`），启动器设置、存档、日志按
  nixport 的 storage 约定统一到 `storage.home`；
- SDL 手柄热插拔扫描在 Android 上被 SELinux 拒绝 → 抑制，避免刷屏；
- MSAA render target 不再无条件走 `eStorage`；
- Tk 启动器在会话内可用（`launcher.sh` + 显式 `TCL_LIBRARY` / `TK_LIBRARY`）；
- `config` / `info` verb 做成动态 schema 的真值契约（宿主靠它列能力）；
- 直装改成**两阶段原子替换**（tar 落盘 + 文件数校验 + 旧树 rename），避免运行中读到半铺文件。

**3）FEX 路径下的功能重做**

上游的作弊、倍率、敌血条、QTE 提示钩子、暂停菜单，靠「往 guest 内存写跳板 / 陷阱」实现；
FEX 执行 guest 代码时这套机制不同，于是这些功能在 FEX 路径上**重做**了一遍：

- **Automatic QTE**（timed / repeat / charged / circle 四类提示）：FEX 陷阱路径 + 补齐按压额度；
- **暂停菜单的 "Exit to Desktop"**；
- **敌血条 / 伤害倍率 / 球倍率 / 红球倍率**；
- 金手指开关（读的是值而不是地址）。

**4）Port 包契约与虚拟按键布局**

- `port.json` 声明 `"virtualKeys": "virtualkeys.json"` ⇒ 宿主导入时把包根那份布局物化成
  **这个 Port 的每游戏默认布局**（用户显式选择优先于包声明）；
- 布局按**横屏手机的握持**重排（不是照抄 PS 预设）：17 个控件，左右拇指各管一块，
  肩键给食指，四个功能键在一掌之内；摇杆固定不浮动 ⇒ 控件之间留出间隙，命中框互不重叠；
- 端口自己的叠加层（画质/作弊菜单、奖杯列表、帧率列表）用**键盘键**做入口
  （`KEY_INSERT` / `KEY_F8` / `KEY_F9`）——这些键游戏永远收不到，不会误触；
- 「斯巴达之怒」默认走**独立一路输入**（`rage_one_input`，默认中键），端口内部再展开成 L3+R3。

细节见 [`packaging/arm-linux/README.md`](packaging/arm-linux/README.md)。

**5）AOT 支线（`feature/aot-native-arm64`，未合并）**

把游戏的 PS4 x86-64 代码**一次性重编译成原生 ARM64**，让游戏不再依赖 FEX JIT：

- 重编译器编译出 `libgow3aot.so`（零 lift 失败、零未支持 op），真机可加载；
- 钩子机制从「写跳板 / 陷阱」换成**编译期插入的 hook slot**（运行期只填 slot，不改游戏代码）；
- 已通：暂停菜单 Exit to Desktop、金手指开关、敌血条、伤害/球倍率；
- 未通：Automatic QTE 四类里的 timed / charged / circle 所在的代码路径没被走到；
  `install_qte_hooks` 在 AOT 分支未被调用 ⇒ 提示序列机制没装上；HEALTH 金手指未做；
  设备侧自带编译器（bionic `bb-recomp`）未做。

**6）迁移与回归工具**

- `tools/check_migration.py` + `tests/migration/`：把 ARM64 线与参考实现逐函数比对；
- `tests/test_qte_fex_stub.c`、`tests/test_pause_menu.py`、`tests/test_port_display_fit.py` 等新测试。

### 当前状态与已知限制

**已验证可用**

- ARM64 Android 上真机跑通：进游戏、过场动画（H.264）、游戏画面 60 fps、存档读写；
- 屏上虚拟按键布局按设计渲染（图标 + 英文标签），面部四键在拇指落点一掌之内；
- 端口叠加层的奖杯列表、帧率列表可用；
- Automatic QTE 在 **FEX 路径**上已装上全部位点（详见 openspec-hub 专栏第 16 篇）。

**已知限制**

- **AOT 支线未合并、未发布**：Automatic QTE 在 AOT 下无效，HEALTH 金手指未做；
- 触摸板与拍照模式在安卓上**不上屏**（没有对应硬件，也不是这个游戏要的东西）；
- x86-64 桌面构建本轮未做回归（ARM64 线改动没有动上游 `build.sh`，但仍需验证）；
- 需要 Vulkan 1.3 驱动（Android 侧建议 Turnip）与宿主 rootfs 的 glibc；
- 上游的 Steam 集成、Discord 状态、Windows 叠加层等桌面特性在 ARM64 Android 上不适用。

### 分支说明

| 分支 | 内容 | 状态 |
|---|---|---|
| `runtime-arm64` | ARM64 端口线：交叉构建、Port 打包、虚拟按键布局、FEX 路径的功能重做 | **本分支（发布线）** |
| `feature/aot-native-arm64` | 在端口线之上加 AOT 原生重编译（hook slot 机制） | 实验中，**未合并** |
| `main` | 上游 SpartanPort 原样（落后上游） | 只作基线 |

> 两条线此前是分叉的，现已把端口线合并进 AOT 分支；本 README 描述的是 `runtime-arm64`。

---

## 1. 下载与游玩

### 1.1 ARM64 Android（本分支，主线）

**你需要：**

- 一台 ARM64 Android 设备（本项目在 1904×3040 的平板上验证）；
- 已装好 **nixport** 宿主（提供 rootfs、rootfs 的 glibc、GPU 驱动与会话管理）；
- 你自己的游戏 dump（`eboot.bin` / `sce_module` / `sce_sys` 齐全的目录）；
- Android 侧可用的 Vulkan 1.3 驱动（推荐 Turnip）。

**步骤**

1. 从本分支的构建产物拿到 `dist/spartanport-port.zip`（包根 = `port.json` + 恰好一个
   `SpartanPort-arm64.AppImage` + 可选的 `virtualkeys.json`）；
2. 把 zip 传到设备（`/sdcard/Download/`），在 nixport 的「库 → 导入 Port 分发包」里选中它；
3. 宿主导入后，这个 Port 的目录落在 `files/imports/<port.json 的 id>/`，虚拟按键布局自动
   物化成它的每游戏默认布局；
4. 进 Port 的启动器（条目只有一个：`SpartanPort 启动器`）→ 选游戏目录 → 保存 → 开始游戏。

**玩的时候**

- 屏上控件就是操作本体：左摇杆移动、左上角 L1/L2、右上角 R1/R2、右侧 △○×□ 四键、
  方向键切四把主武器；
- ⚙ 打开端口菜单（画质/作弊/倍率/补丁），🏆 打开游戏内奖杯列表，⏱ 打开帧率列表；
- 「怒」键单独一个，按下即触发斯巴达之怒。

**存档与日志**

- 存档、着色器缓存、日志在 Port 目录的 `home/gport/user/` 下（`savedata/` 是存档），
  升级时保留这个目录即可；
- 卸载 Port 时宿主会连带清理它物化的布局。

### 1.2 Windows 10/11（上游）

以下内容来自上游 README，适用于**上游发布包**，本分支不发布 Windows 二进制。

- Windows 10（1903 或更新）或 Windows 11，64 位。
- 带 AVX 的 64 位处理器：Intel Sandy Bridge（2011）之后或 AMD FX 之后。没有
  MOVBE、BMI1 或 LZCNT 的 CPU（Sandy/Ivy Bridge、AMD FX）上，加载器会把游戏用到这些指令的
  代码改写成模拟桩，并在日志里记一行 `ISA:`；这些 CPU 的性能还没测过。更老的 CPU 会在启动时
  停在 `STOP: this CPU lacks`。
- 支持 Vulkan 1.3 且驱动较新的显卡。
- 你自己的游戏副本（含 `eboot.bin`、`sce_module`、`sce_sys` 的目录）。测试过：CUSA01623 与
  CUSA01715，版本 01.00 与 01.02。

**步骤**

1. 从[发布页](https://github.com/FelipePrado0/SpartanPort/releases/latest)下载 `spartanport-windows.zip`。
2. 解压到任意目录，例如 `C:\Games\spartanport-windows`。不要在压缩包里直接运行。
3. 打开 **`SpartanPort.exe`**。若 Windows 提示「Windows 已保护你的电脑」，点
   **更多信息** → **仍要运行**（程序没有数字签名）。
4. 点 **Choose the game folder**（选择游戏文件夹），会跳到 **Game** 页，在 **Game folder**
   旁边点 **Browse…**，选含 `eboot.bin` 的目录。
5. 回到 **Home**，会显示封面与「CUSA01623 · version 01.02」。想改就改 **Quick settings**
   （显示模式、帧率上限、渲染分辨率、引擎帧率）。
6. 点 **Save** 保存待改项，然后点 **PLAY**。首次启动要准备游戏镜像；着色器编译也会影响
   之后的启动速度与性能。

补丁与修复已内置并自动生效：纹理修复、跳过过场动画、X 与 120 FPS 默认开启。渲染分辨率补丁
会替代纹理修复。

**下次再玩**

- **`Play SpartanPort.exe`** 直接带着已保存的设置启动游戏，可以做桌面快捷方式，或用
  **Add a Non-Steam Game** 加进 Steam。
- 存档、着色器缓存与日志在 `SpartanPort.exe` 同级的 `user` 目录，升级时保留它。
- **Advanced → Check for updates** 下载并安装新版本，保留存档与设置。

### 1.3 Linux x86-64 AppImage（上游，实验性）

Linux 构建跑同一套原生运行时与同一个启动器，在 Arch Linux + Radeon RX 7800 XT（RADV）+
Wayland 上测试过。

**你需要：** glibc 2.39 或更新的 x86-64 Linux、带 AVX 的 CPU、Vulkan 1.3 驱动，以及自己的
游戏 dump。启动器启动在 Ubuntu 24.04、Debian 13、Fedora 42、Arch 与 openSUSE Tumbleweed
上检查过；这不代表每张 GPU 上都能正常游玩。

**下载与运行**

```bash
chmod +x spartanport-x86_64.AppImage
./spartanport-x86_64.AppImage
```

选游戏目录、保存设置、点 **PLAY**。若旧下载报 `GLIBC_2.44 not found`，重新下载重建过的
AppImage。AppImage 会自我更新：**Advanced → Check for updates**（或启动器弹出提示时）从发布页
下载新版、校验 SHA-256 并替换文件；目录不可写时会打开发布页。

- **文件：** 存档、设置、着色器缓存与日志在 `~/.local/share/spartanport`；启动器自己的设置在
  `~/.config/spartanport-launcher`。
- **Advanced → Linux：** 窗口系统（自动 / Wayland / X11）、GameMode、MangoHud；只有系统里装了的工具才能开。
- **Steam：** **Advanced → Add to Steam** 把「SpartanPort」作为非 Steam 游戏加入，用 dump 里的图
  做图标与胶囊。关掉 Steam 再操作（它退出时会重写快捷方式）。
- **Steam Deck / SteamOS（Gaming Mode）：** 若 Steam 想用 Proton 启动 AppImage，把快捷方式的
  **属性 → 兼容性**改成 **Steam Linux Runtime**。AppImage 是原生 Linux 程序。

---

## 2. 能跑什么

- 开机、菜单、过场动画（H.264）与游戏本体。
- 音频：音乐、语音与音效（ATRAC9 与 MP3）。
- 手柄（DualSense、Xbox 等，经 SDL3）与键盘可同时使用，两者都可改键。
- **游戏内按键图标**（可选，默认关）：可以把 PlayStation 按键图标换成你手柄的、Xbox 或
  Nintendo 的，或换成你实际用的键位（Game 页 → Controls）。
- 存档与读档：存档列表在屏幕上显示，和 PS4 一样；自动存档也可用；启动器每次启动前会备份存档。
- 补丁：渲染分辨率 480p / 720p / 900p / 1440p / 1800p / 4K、损坏纹理修复、跳过过场动画
  （社区补丁）；引擎帧率 120 FPS（kvicken）、100 FPS（100Hz 显示器）、144 FPS（144Hz 显示器）
  或 240 FPS（Felipe Prado 适配）。
- **超宽屏（beta）**：21:9、32:9、16:10 渲染分辨率（测过 3440x1440），是更宽的视野而不是把画面放大；
  过场动画保持 16:9 边框（见 [docs/ultrawide.md](docs/ultrawide.md)）。Mod 也可以自带游戏补丁。
- **后台着色器编译**：存档的管线在你玩的时候后台加载编译；编译期间仍可能卡顿、短暂掉帧。
- **启动器**：游戏配色，多语言（缺英文回退），五个页面（**Home、Graphics、Performance、Game、
  Advanced**）与 **Save** 按钮；首页有快捷设置；还能管游戏目录、存档备份、补丁、Mod（顺序、预设、
  冲突）、按键、更新与诊断。
- **游戏内菜单**（键盘 Insert，或手柄 R3+L2），四个标签页：
  - **Game：** 金手指（无限生命、魔力、道具槽、斯巴达之怒、最大红球、
    自动 QTE：按键与摇杆旋转提示自动应答）；红/绿（生命）/蓝（魔力）/金（斯巴达之怒）球与
    造成/承受伤害的倍率 0.1x–100x；敌血条。
  - **Image：** 显示模式（窗口、无边框、全屏）、VSync、RCAS 锐化。
  - **Performance：** 帧率上限（30 到不限）、排队帧、后台着色器编译；渲染分辨率、引擎帧率、
    GPU 回读选项用一次「应用并重启」生效。
  - **Overlay：** 性能叠加层（FPS、帧时间、放大器、GPU 名称与占用、CPU、内存、显存），各项可选，
    可设字号、角落、背景透明度与布局。数字是游戏自己的占用，不是整机的。

---

## 3. 功能可用性

对照 [release 2.2.0](https://github.com/FelipePrado0/SpartanPort/releases/tag/windows-v2.2.0)
与源码核对（2026 年 10 月 14 日）。

| 功能 | 发布的 2.2.0 包 | 当前源码 |
|---|---|---|
| Windows ZIP / 实验性 Linux AppImage | 有 | 构建脚本可用 |
| 启动黑屏补丁 / 手柄按键图标 | 内置 | 内置 |
| Discord 状态 | 无 | Advanced 下可选，默认关；显示游戏名与时长 |
| RADV 稀疏队列修复 | 无 | 已并入 `main` |
| FSR 1、FSR 3.1、FSR 4、TAA、DLSS | 关闭 | 在启动器/游戏设置路径里关闭 |
| RCAS 锐化 | 有 | 有 |

> ARM64 Android 线以本文档第 0 章的状态表为准。

---

## 4. 设置

设置放在 `spartanport.ini`（游戏与游戏内菜单）和 `settings.json`（启动器）里，都在端口目录下。
存档、着色器缓存与 `last_run.log` 放在项目的 `user` 目录。

**GUI 缩放：** 在 **Advanced → Launcher** 里选 **Auto** / **1x** / **2x**，然后点 **Save**。
文字、控件与布局一起缩放，不改游戏分辨率。Auto 跟随 Tk 报告的桌面缩放，在 Linux 上有 X11/XWayland
字体 DPI 与窗口缩放兜底；macOS 上 Retina 缩放由系统处理。手动 1x/2x 设置的是启动器的逻辑尺寸
（含文字、图标、控件、滚动条与间距）。选项存在 `settings.json`。

在 **Performance → GPU data → Stream buffer memory** 里可选 **System RAM (cached, default)**、
**System RAM (uncached)** 或 **GPU memory (previous behavior)**；在 NVIDIA 笔记本上系统内存可能提高
帧率。直接启动（不经启动器）时用 `SPARTAN_STREAM_MEMORY` 取 `cached` / `host` / `device`，
经启动器时以启动器保存的选择为准。

在 **Performance → GPU data → Game multisampling** 里，**Automatic** 让游戏按显卡自行决定：
在 AMD 与 Intel 的 Windows 驱动上以单采样创建多重采样图像（把 2 采样深度缓冲当纹理读会在光照里
产生黑线，出现在 Radeon RX 6600 系与 Arc 上），其余显卡保持多重采样。选项会显示具体决定，例如
「Automatic (Intel(R) Arc(TM) A750 Graphics: off)」，并跟随 **Graphics card** 的选择。
关/开分别设置 `SPARTAN_NO_MSAA=1` / `0`；Automatic 会透传继承来的 `SPARTAN_NO_MSAA`。下次启动生效。

### 中文界面

在 **Advanced → Launcher language** 里选 **简体中文**，启动器与 Insert / R3+L2 菜单都会变中文；
不影响游戏的文本与语音语言。直接启动时用 `SPARTAN_UI_LANGUAGE=zh`。

启动器优先用系统已装的 CJK 字体（Windows 的 Microsoft YaHei UI，其他平台的 Noto Sans CJK SC 等）。
游戏内菜单把系统里的中文字体与内置字体合并：Windows 的雅黑/黑体、Linux 常见的 Noto/文泉驿路径、
macOS 的苹方。不额外分发字体；找不到可用 CJK 字体时菜单保持英文，并在日志里记原因。

无头 CMake 目标 `chinese-ui-test` 在没有游戏 dump、没有 Vulkan 设备的情况下检查翻译回退、printf
参数、字体合并与中文字形；系统没装 CJK 字体时会跳过真实字形检查并给出提示。

---

## 5. 键盘操作

默认值；可在启动器 **Controls** 页的 **Keyboard & Mouse Mapping** 里改。

| 键 | 作用 |
|---|---|
| WASD | 左摇杆 |
| 方向键 | 右摇杆（视角） |
| 空格 | Cross |
| 左 Shift | Circle |
| 左键 | Square |
| 右键 | Triangle |
| Q / E | L1 / R1 |
| R / 鼠标前进键 | L2 / R2 |
| Z / X | L3 / R3 |
| 滚轮中键 | 斯巴达之怒（L3 + R3） |
| C | 抓取（L1 + Circle） |
| 鼠标后退键 | 闪避 |
| 1、2、3、4 | 方向键上、右、下、左（武器） |
| Tab | Options |
| P | 左触摸板点击（拍照模式） |
| Esc | 右触摸板点击（暂停菜单） |
| Insert | 端口菜单（手柄上等效 R3+L2） |

**单键触发斯巴达之怒**（默认开）会让 Z 与 X 闲置。**摇杆提示**不需要摇杆就能应答那些摇杆旋转的
QTE：直接输入转向（半圈 D S A、A S D、A W D 或 D W A；整圈 D S A W 或 A S D W），或用
*Adaptive mouse* 划出来。游戏时鼠标留在游戏窗口里；游戏内菜单、暂停键（Esc）与其他窗口会把鼠标
拿回来，再按一次 Esc 或点一下窗口就回去。按住 L2（R）时鼠标用右摇杆瞄准，用于「赫利俄斯的头颅」
或「阿波罗之弓」：*Relative* 跟随移动，*Absolute* 指向按下 L2 时鼠标所在的位置，
*Right stick sensitivity* 决定需要多少鼠标位移。不经启动器时游戏用自己的键位：E 与 Q 对应
Square 与 Triangle，1 与 3 对应 L1 与 R1，F 对应 R2，C 对应 R3，I/K/J/L 对应方向键，
Enter 对应 Options，Tab 与 Backspace 对应触摸板。

没有触摸板的手柄（Xbox 等）上，View 打开暂停菜单（右触摸板点击），L1 + View 打开拍照模式（左触摸板
点击）。DualSense 上触摸板与 PS4 一样，Share/Create 等同 View。

**游戏内按键图标**（默认关）在启动器 Controls 页的 *In-game button icons* 里选：
*Match my controller*、*Xbox*、*Nintendo* 或 *Keyboard*。它们替换教程、连招表与 QTE 提示里的
PlayStation 图标；手柄样式还会替换「火焰殿堂」里缪斯琴上的图标，暂停菜单的手柄图仍是 DualShock 4。
*Keyboard* 会按你的键位显示按键（改了键位下次启动才生效）；*Match my controller* 在没连手柄时
也显示键位。启动器会往端口数据目录写最多三个游戏文件的补丁副本（约 410 MB），**不会改你的游戏目录**。
不经启动器时，在 `run.py` 前设 `SPARTAN_GLYPH_STYLE` 为 `auto`、`xbox`、`nintendo` 或 `keyboard`。

菜单里：L1/R1 或 Q/E 切标签页。存档列表里：方向键或十字键选择，Enter 或 Cross 确认，
Esc 或 Circle 取消。

---

## 6. 导入 shadPS4 存档

只有**本游戏的 shadPS4 存档**兼容。RPCS3 的存档来自 PS3 版，不能用。要用同一个 title ID
（下面用 `CUSA01623` 举例）。

1. 关掉游戏与 shadPS4，先备份两个存档目录。
2. 找 shadPS4 数据目录里的源：旧版本在 `user\savedata\1\CUSA01623\`，新版本在
   `home\<user-id>\savedata\1\CUSA01623\`。
3. 目标是 **`SpartanPort.exe` 同级的 `user\savedata\1\CUSA01623\`**，例如
   `C:\Games\spartanport-windows\user\savedata\1\CUSA01623\`。若在启动器里自定义了 **Saves folder**，
   用 `<自定义目录>\savedata\1\CUSA01623\`。
4. 把每个存档槽目录复制过去，保留目录名与文件；不要再套一层 `CUSA01623`；不要把槽内的
   `sce_sys` 目录算进游戏数据。
5. 每个存档槽旁边建 `<槽名>.sce_sys`（若没有）。可以从 shadPS4 的 `sce_sys\icon0.png` 复制。
   **不要把 `param.sfo` 改名成 `param.bin`**：两种格式不同。已存在的端口 `param.bin` 要保留。
   没有它时存档槽只按目录名显示；在端口里再存一次就会写入它自己的元数据。
6. 启动端口并选 **Load Game**。已完成的存档也能导入。

端口的存档对话框最多显示 **32** 条。若长列表里少了某个槽，用一个只含该槽（外加同级元数据目录）
的单独 **Saves folder** 试一次。保留原存档，测完把 Saves folder 设置改回去。

### 用存档压缩包（zip）导入

启动器自己就能导入：**Game → Import saves from a zip**（导入存档）。压缩包里的存档布局与
端口自己保存的一致：

```
savedata/1/<TITLE_ID>/<SLOT>/SAVEDATA
savedata/1/<TITLE_ID>/<SLOT>.sce_sys/param.bin
savedata/1/<TITLE_ID>/<SLOT>.sce_sys/icon0.png
```

导入会**整体替换该 title id 的目录**：压缩包里的槽位成为这个游戏的槽位，包里没有的槽位会消失。
导入前先把当前存档备份一份（见下面的 **Save backups**），所以可以撤销。若压缩包的 title id 与
游戏目录不一致，启动器会先提示，由你决定是否仍然导入。

在 Android（Nixport）上，文件是在宿主的触屏选择屏里挑的：先把 zip 放进手机存储（一般是「下载」）
再选它。

---

## 7. 第三方叠加层（Windows）

游戏进程是 **`bin\spartanport-probe.exe`**，不是 `SpartanPort.exe`（源码构建是
`out\spartanport-probe.exe`）。把该可执行文件加进 RTSS 并启用 Afterburner 的屏显。
ReShade 安装器里选同一个可执行文件与 **Vulkan** API。

若叠加层还是不出现，请报告端口与叠加层版本、上一个能用的端口版本，并附上 `user\last_run.log`。
这些排查不能保证解决所有叠加层配置的问题。

---

## 8. 排错

- 可执行文件格式报错：源码构建接受明文 PS4 SELF 容器与已解包的小端 x86-64 ELF 可执行文件与模块；
  以前有效的已解包 ELF 会被误报 `prepare failed: expected PS4 SELF`。其他格式的文件，以及
  加密/压缩的 SELF 段，仍需要正确准备过的游戏 dump。
- 启动黑屏：用最新发布版（含启动死锁补丁）。若仍存在，请报告 GPU、驱动、游戏版本与
  **Advanced → Log** 的输出。清着色器缓存不是通用解法，且下次启动要重建。
- 游戏以「cannot create ... guest memory (error 1450)」退出：Windows 虚拟内存不够分配 guest 池。
  增大页面文件：`Win + R` → `SystemPropertiesAdvanced` → **性能**设置 → **高级** → **虚拟内存**更改，
  取消自动管理，选 **C:** 盘，自定义大小（如初始 16384 MB、最大 24576 MB），点 **设置**、**确定**，
  重启电脑。
- 启动器里的 **Game language** 没效果：它只在游戏首次启动时用一次。之后游戏会从自己的设置存档
  恢复文本与语音语言，要改就在游戏里：**Options → Game Settings → Game Text** 与
  **Options → Audio → Speech**。
- 游戏打不开：看 **Advanced → Log**，或把 `user\last_run.log` 附在报告里。若自定义了
  **Saves folder**，`last_run.log` 在那个目录里。
- 黄色/橙色/白色纹理或光照不对：先试 **Performance → GPU data → Deferred GPU readbacks** 关掉，
  再重启。这个绕法可能引起卡顿。
- 特定 GPU 的渲染失败、崩溃、音频问题与卡进度仍在排查中。报告前先看
  [发布说明](https://github.com/FelipePrado0/SpartanPort/releases/latest)与
  [issues](https://github.com/FelipePrado0/SpartanPort/issues)，并附上问题发生前的存档。

---

## 9. 性能与兼容性

此前在 AMD Radeon RX 6700 XT、Ryzen 5 5600X、32 GB 内存上测得：1080p + 240 FPS 引擎帧率下
平均 144 FPS（55–240）。这是参考结果，不是每个版本或每台机器的基准。

测试机上 1080p 原生分辨率 + 240 FPS 引擎帧率可从头玩到尾。其他配置上也报告过纹理损坏、崩溃与
卡进度，包括 1080p。已知问题见发布说明。

> ARM64 Android 侧：验证机（1904×3040 平板）上游戏以 540p / 960×540 直通方式运行，帧率约 60。
> 这不是性能承诺。

---

## 10. 从源码构建

### 10.1 ARM64 Android（本分支）

```bash
bash packaging/arm-linux/build.sh      # 交叉构建 + 组包 + verb 冒烟 → packaging/arm-linux/dist/spartanport-port.zip
bash packaging/arm-linux/deploy.sh     # adb push 到 nixport debug app（真机调试）
```

前置（宿主状态，首次自动准备）：

- sysroot：`SPARTAN_ARM64_SYSROOT` 未设且本机/参考仓（`../gow_android_glibc`）都没有时，
  自动跑 `fetch-sysroot.py`（Debian trixie arm64，SHA256 校验）；
- 宿主头文件：tsl-robin-map / VMA / glslang（`SPARTAN_ARM64_HEADERS`，缺省复用参考仓）；
- macOS：brew `llvm` / `lld` / `boost`；Linux：系统 llvm 工具链。

产出的 zip 包根恰含 `port.json` 与恰好一个 `SpartanPort-arm64.AppImage`（外加可选的
`virtualkeys.json`），宿主导入时自动物化布局。细节见
[`packaging/arm-linux/README.md`](packaging/arm-linux/README.md)。

**AOT 支线**（仅 `feature/aot-native-arm64` 分支）：

```bash
bash tools/aot/build-recompiler.sh     # 准备重编译器 + glibc 工具链（一次）
bash tools/aot/compile-game.sh          # prepare → link_modules → AOT 编译
```

### 10.2 Windows（上游）

安装 python.org 的 Python 3.10 或更新版本（带 Tk 支持），然后在 PowerShell 里装启动器与
指令改写依赖：

```powershell
python -m pip install pillow iced-x86==1.21.0
```

下面的构建命令用 MSYS2 CLANG64；标了 PowerShell 的启动器命令用 Windows 的 Python。

开发在 `spartanport` 分支上进行；`main` 只接收经过测试的合并。

1. 装 MSYS2（`winget install MSYS2.MSYS2`），在 **MSYS2 CLANG64** shell 里装
   [packaging/windows/README.md](https://github.com/FelipePrado0/SpartanPort/blob/main/packaging/windows/README.md)
   列出的包。
2. 克隆仓库（含子模块）并构建：

   ```bash
   git clone --recursive https://github.com/FelipePrado0/SpartanPort
   cd SpartanPort
   git checkout spartanport   # 开发分支；构建 main 时省略
   bash build.sh
   ```

3. 启动启动器（PowerShell，在项目目录里）：

   ```powershell
   python launcher/spartanport_launcher_win.py
   ```

4. 在 **Game** 页选游戏目录与补丁。调整 **Graphics** 与 **Performance**，
   点 **Save** 保存待改项，然后 **PLAY**。

不打开启动器窗口、带着保存的设置直接玩（快捷方式或 Steam）：

```powershell
python launcher/spartanport_launcher_win.py --play
```

完全不经启动器：

```powershell
$env:SPARTAN_GAME_DIR = 'D:\path\CUSA01623'
python run.py
```

### 10.3 Linux x86-64（上游）

全新检出的，在 Linux shell 里跑：

```bash
git clone --recursive https://github.com/FelipePrado0/SpartanPort
cd SpartanPort
bash packaging/linux/ci-deps.sh
bash build.sh
out/venv/bin/python launcher/spartanport_launcher_win.py
```

构建后再打 AppImage：

```bash
PATH="$PWD/out/venv/bin:$PATH" bash packaging/linux/build.sh
./dist/spartanport-x86_64.AppImage
```

依赖脚本用 sudo 装系统包、选 GCC 14 为默认编译器、创建含 PyInstaller、Pillow 与 iced-x86 的
`out/venv`。它面向 Ubuntu 24.04；别的发行版要改依赖。在较新的发行版上构建可能需要更新的 glibc；
公开发布用 Ubuntu 24.04 构建。从仓库直接用
`out/venv/bin/python launcher/spartanport_launcher_win.py --play` 走保存的启动器设置，或
`SPARTAN_GAME_DIR=/path/to/CUSA01623 out/venv/bin/python run.py` 直接启动。

---

## 11. 测试

在仓库根目录、构建之后运行。下面的 C++ 检查不需要游戏 dump。

### Python

Windows 上用 PowerShell；Linux 上把 `python` 换成 `out/venv/bin/python`。

```powershell
python -m unittest discover -s tests -p "test_*.py"
```

### Windows C++ 测试（MSYS2 CLANG64）

```bash
source ./msys2-env.sh
ninja -C out/gpu perf-overlay-test warmup-inbox-test graphics-settings-test
out/gpu/perf-overlay-test.exe
out/gpu/warmup-inbox-test.exe
out/gpu/graphics-settings-test.exe
```

### Linux C/C++ 测试

```bash
bash build.sh --test
ninja -C out/gpu perf-overlay-test warmup-inbox-test graphics-settings-test
out/gpu/perf-overlay-test
out/gpu/warmup-inbox-test
out/gpu/graphics-settings-test
```

`bash build.sh --test` 只在 Linux 上跑额外的 C 运行时测试；Windows 上构建会在那之前退出，
改用上面的 Ninja 目标。

### 用准备好的游戏做检查

准备好自己的 dump 后，下面的检查需要 `out/eboot.elf` 与 `out/boot-linked.bin`。
在 Windows 的 MSYS2 CLANG64 里把示例游戏路径换成你的：

```bash
ninja -C out/gpu stat-multipliers-test
out/gpu/stat-multipliers-test.exe out/eboot.elf
out/spartanport-probe.exe out/boot-linked.bin --app0 "D:/Games/CUSA01623" --check-imports
```

Linux 上用 `out/gpu/stat-multipliers-test` 与 `out/spartanport-probe`（不带 `.exe`）和 Linux 游戏路径。
`--check-imports` 检查系统导入并退出，不进入游戏。更多测试目标见
[gpu/CMakeLists.txt](https://github.com/FelipePrado0/SpartanPort/blob/main/gpu/CMakeLists.txt)。

### ARM64 线的额外测试

```bash
python tools/check_migration.py --help     # 与参考实现逐函数比对
python -m unittest discover -s tests/migration -p "test_*.py"
```

---

## 12. 工作原理

CPU 这一侧原本是原生的：游戏的可执行文件直接跑在 PC 的处理器上（PS4 用的是同一套 x86-64 架构），
一个为这个游戏写的运行时替换掉 PS4 的系统库，不做 CPU 模拟。图形则被翻译：跑在
[shadPS4](https://github.com/shadps4-emu/shadPS4) 视频核心里，把 PS4 的 GPU 命令与着色器转成
Vulkan，并带上这款游戏的渲染修复（来自
[cuesta4 的 `eltutz` 分支](https://github.com/cuesta4/shadPS4/tree/eltutz)）。直接在 CPU 上跑游戏
代码也是 shadPS4 的做法；这个项目改的是系统层与游戏相关的工作（补丁、后台着色器缓存、启动器与
游戏内菜单）。它是 [Supermedo 的 Windows 移植](https://github.com/Supermedo/bloodborne_pc)
（源自 [deadinside28 的 bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)）的分支
（见[致谢](#13-致谢)）。

- 补丁与游戏内钩子（金手指、倍率、敌血条）靠**签名**找位点：每个位点周围那些原始字节，
  无论发布版把代码放在哪里都能找到（`tools/make_signatures.py`）。找不到位点时，
  只有那个补丁/钩子不装；游戏照常启动，日志与菜单会说明原因。

| 部件 | 位置 | 作用 |
|---|---|---|
| 准备 | `scripts/` | 读 `eboot.bin`，处理重定位，把游戏自己的 `libc` 与 `libSceFios2` 链成一个镜像 |
| 加载器 | `src/probe.c` | 加载镜像，装上校验过的钩子（金手指、倍率、敌血条），启动游戏原生代码 |
| 运行时 | `src/runtime_*.c` | 重实现 PS4 系统函数：内存、线程、文件、音频、视频、手柄、存档与服务 |
| GPU | `gpu/shadps4/` | shadPS4 视频核心，把 PS4 GPU 命令与着色器转成 Vulkan，带这款游戏的修复与本端口的性能工作 |
| 游戏内菜单 | `gpu/shim/spartanport_overlay.cpp` | Dear ImGui 菜单与性能叠加层；设置在 `gpu/shim/spartanport_settings.cpp` |
| 补丁 | `patches/CUSA01623.xml`、`scripts/patches.py` | 启动时在内存里应用，不改游戏文件 |
| 启动器 | `launcher/spartanport_launcher_win.py` | 设置、补丁、Mod、按键与开始按钮 |

更多文档：[mods](https://github.com/FelipePrado0/SpartanPort/blob/main/docs/MODS.md)、
[patches](https://github.com/FelipePrado0/SpartanPort/blob/main/docs/PATCHES.md)、
[帧映射与放大器研究](https://github.com/FelipePrado0/SpartanPort/blob/main/docs/upscaler.md)
与[并行 GPU 工作](https://github.com/FelipePrado0/SpartanPort/blob/main/docs/parallel_gpu.md)。

**ARM64 线额外的一层**：guest x86-64 代码由 **FEXCore** 执行（FEX 的 x86-64 模拟/优化器，
`third_party/FEX`），所以运行时与 GPU 侧的「写跳板/陷阱」钩子机制在 ARM64 上要按 FEX 的语义重做；
AOT 支线更进一步，直接把游戏代码重编译成原生 ARM64，用编译期插入的 hook slot 传递钩子。

---

## 13. 致谢

本项目是 Supermedo 的 Windows 移植的分支，后者又建立在 deadinside28 的原始移植上：

- **deadinside28**：[bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)，最初的原生移植。
- **Supermedo**（Mohammed Albarghouthi）：[bloodborne_pc for Windows](https://github.com/Supermedo/bloodborne_pc)，
  本项目的来源分支。
- **cuesta4**：[专注这款游戏的 shadPS4 分支](https://github.com/cuesta4/shadPS4/tree/eltutz)
  （分支 `eltutz`）。本移植的渲染依赖它的修复：GNM 混合重写、深度/模板状态与图像别名处理。
  cuesta4 也是分辨率、纹理修复、120 FPS 与跳过过场动画补丁的共同作者（见下）。用 shadPS4 的话，
  这才是这款游戏该用的构建。
- **shadPS4 团队**：[shadPS4](https://github.com/shadps4-emu/shadPS4) 视频核心，本移植里所有 GPU 与
  着色器翻译都由它完成。
- **[WilkPrintes](https://github.com/WilkPrintes)**：设计并搭建了
  [SpartanPort 官网](https://spartanport.com/en)。特别感谢他给了这个项目一个家。

补丁：kvicken 与 cuesta4（分辨率、纹理修复、120 FPS 与跳过过场动画，基于 illusion0001 的 PS3 补丁），
发布在 [shadps4-emu/ps4_cheats#145](https://github.com/shadps4-emu/ps4_cheats/pull/145) 并适配到本
可执行文件。144 与 240 FPS 补丁由 Felipe Prado 从 120 FPS 补丁适配，100 FPS 版是同一补丁改数值。
金手指改编自 Celogamez 的 [GoldHEN cheats](https://github.com/GoldHEN/GoldHEN_Cheat_Repository)。

还用到：[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9)、[FFmpeg](https://ffmpeg.org)、
[SDL3](https://github.com/libsdl-org/SDL)、[Dear ImGui](https://github.com/ocornut/imgui)、
[sirit](https://github.com/shadps4-emu/sirit)、[magic_enum](https://github.com/Neargye/magic_enum)、
[miniz](https://github.com/richgel999/miniz)、[xbyak](https://github.com/herumi/xbyak)、
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)、
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) 与 AMD FidelityFX SDK、
[MSYS2](https://www.msys2.org)、[LLVM](https://llvm.org)、[Pillow](https://python-pillow.org)。

**ARM64 线的额外致谢**

- [FEX](https://github.com/FEX-Emu/FEX)：FEXCore —— ARM64 上执行 guest x86-64 的那一层。
- `Bloodborne-android-rebuild-tool`（`bb-recomp`，GPL-3.0-or-later）：AOT 重编译器的参考工程
  （本仓只调用它的命令行，不复制其源码；本地作为参考仓使用）。

---

## 14. 许可

GNU GPL v2 或更新（[LICENSE](https://github.com/FelipePrado0/SpartanPort/blob/main/LICENSE)）。
第三方组件保留各自的许可。AOT 支线参考的 `bb-recomp` 是 GPL-3.0-or-later，本仓源码头部是
`GPL-2.0-or-later`（可升到 GPL-3）。
