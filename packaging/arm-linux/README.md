# packaging/arm-linux — ARM64 Linux 构建与 fexdroid Runtime 打包

把 SpartanPort 交叉构建为 **ARM64 Linux 原生程序**（GPU 原生 + guest x86_64 经 FEXCore），
并组装成 fexdroid 的 **Port 分发包**（`.zip`：包根恰含 `port.json` 与恰好一个 `.AppImage`，
契约见 fexdroid 仓 `my-docs/移植规范/Port打包规范.md`；归档形态只有 `.zip` 一种，
tar/tar.gz 不再是导入通道）。

与 `packaging/linux/`（x86_64 AppImage）同构：同一套构建步骤的交叉变体，程序树采用上游
packaged 布局（`bin/spartanport-probe`、`bin/gpu/`、`run.py`、`scripts/`），差异只有三点：

1. 工具链 = clang `--target=aarch64-linux-gnu` + Debian arm64 sysroot（`fetch-sysroot.py` 产出）
2. 跳过 x86 宿主专属源（`isa_emulate/cpu_check/isa_rewrite`——guest x86 代码由 FEXCore 执行）
3. probe 链接时烤入 fexdroid rootfs 的 ELF 解释器（Android 沙箱没有 `/lib`）

## 用法

```bash
bash packaging/arm-linux/build.sh      # 构建 + 组包 + verb 冒烟 → dist/spartanport-port.zip
bash packaging/arm-linux/deploy.sh     # adb push 到 fexdroid debug app（真机调试）
```

前置（宿主状态，首次自动准备）：

- sysroot：`SPARTAN_ARM64_SYSROOT` 未设且本机/参考仓（`../gow_android_glibc`）都没有时，
  自动跑 `fetch-sysroot.py`（Debian trixie arm64，SHA256 校验）
- 宿主头文件：tsl-robin-map / VMA / glslang（`SPARTAN_ARM64_HEADERS`，缺省复用参考仓）
- macOS：brew `llvm` / `lld` / `boost`；Linux：系统 llvm 工具链

## 文件

| 文件 | 职责 |
|---|---|
| `build.sh` | 交叉构建（FEXCore/gpu/probe）→ 依赖库打包 → 程序树组装 + 验收 + verb 冒烟 |
| `cross-toolchain.cmake` | gpu/FEX 的 CMake 交叉工具链（CMAKE_SYSROOT + llvm-ar 修复） |
| `fetch-sysroot.py` | Debian trixie arm64 sysroot 拉取（SHA256 锁定） |
| `entry.sh` | 程序树入口：fexdroid 宿主环境块 + config/info 契约 + `exec run.py` |
| `runtime.json` | fexdroid manifest v1（entries/capabilities/requires/storage） |
| `deploy.sh` | 真机调试推送（adb + run-as） |

## 依赖库策略

与 `packaging/linux/` 同源：打包 "运行宿主没有" 的库（SDL3、ffmpeg、libgportgpu/cpu…），
排除清单 = rootfs 标配（glibc、Vulkan loader、X11、PulseAudio、fontconfig……）。在
fexdroid 上，这份清单恰好就是 rootfs 的 `RUNTIME_PKGS`——rootfs 是公共地基，程序树是自带行李。

## 虚拟按键布局（`virtualkeys.json`）

包内带一份虚拟按键布局（宿主规范 `nixport.virtualkeys.v1`），`port.json` 的 `virtualKeys`
指向它 ⇒ 导入后自动物化成**这个 Port 的每游戏默认布局**（用户显式选择优先）。布局文件放在
**包根**（与 `port.json` 同级），不在 AppImage 里——宿主导入时从包根读。

### 为什么不是「照抄 PS 预设」

宿主内置的 PS 预设是给「通用手柄」用的：肩键贴着屏幕最上沿，四键在右摇杆左侧。手机上
肩键那条上沿**手指够不到**（且横屏时中间是刘海区），四键离拇指落点也偏远。本布局按
**横屏手机的握持**重排：

| 区域 | 控件 | 说明 |
|---|---|---|
| 左拇指 | 左摇杆（0.13, 0.655）、方向键（0.135, 0.395）、怒（0.115, 0.865） | 摇杆是落点；方向键与怒是「滑动一下」的距离（229 / 281 px） |
| 左食指 | L2（0.085, 0.070）、L1（0.085, 0.185）、菜单/奖杯/帧率（上排 0.185–0.305） | 食指沿左上角横移 |
| 右拇指 | ×75px、○148px、△248px、□251px（相对落点 0.75, 0.78） | 四个功能键都在一掌之内；×（跳）最近 |
| 右食指 | R2（0.915, 0.070）、R1（0.915, 0.185）、暂停（0.915, 0.300） | 右手食指竖排 |

摇杆是**固定**的（宿主按控件中心算向量，不浮动）⇒ 控件之间必须留出间隙；2400×1080 下
实测各控件的命中框**互不重叠**。

### 键位映射（宿主绑定 → 游戏里的 PS4 键）

宿主的 `GAMEPAD_*` 绑定经端口的 SDL 映射落到游戏的 PS4 键（`src/runtime_pad.c`）：

| 屏上 | 宿主绑定 | 游戏里的键 | 作用 |
|---|---|---|---|
| □ | `GAMEPAD_BUTTON_X` | Square | 轻攻击（核心输出、抓取时举敌冲锋） |
| △ | `GAMEPAD_BUTTON_Y` | Triangle | 重攻击（可破防、长按挑空、抓取时处决） |
| ○ | `GAMEPAD_BUTTON_B` | Circle | 抓取 / QTE 处决 / 场景交互 |
| × | `GAMEPAD_BUTTON_A` | Cross | 跳跃 / 二段跳 / 滑翔 / 蹬墙 |
| L1 | `GAMEPAD_BUTTON_L1` | L1 | 格挡、弹反（按住配 □/△/○/× 出派生） |
| R1 | `GAMEPAD_BUTTON_R1` | R1 | 场景交互（开箱、调查、滑墙、机关） |
| L2 | `GAMEPAD_BUTTON_L2` | L2 | 按住用副武器/道具 |
| R2 | `GAMEPAD_BUTTON_R2` | R2 | 当前武器的魔法 |
| 方向键 | `GAMEPAD_DPAD_*` | ↑↓←→ | 切 4 把主武器（游戏菜单也用它走列表） |
| 暂停 | `GAMEPAD_BUTTON_START` | OPTIONS | 游戏的暂停菜单 |
| 左/右摇杆 | `GAMEPAD_LEFT/RIGHT_THUMB_*` | 摇杆 | 移动奔跑 / 视角闪避切锁定 |

**触摸板与拍照模式不上屏**：PS4 触摸板在安卓上没有对应物，拍照模式也不是这个游戏要的东西。

### 三个「只给宿主用」的键

端口自己的叠加层（画质/作弊菜单、奖杯列表、帧率列表）在**键盘键**上也有入口，且这些键
**游戏永远收不到**（端口的键位表里没有它们）⇒ 屏上单键即可，不会有副作用：

| 屏上 | 绑定 | 端口里的含义 |
|---|---|---|
| 菜单 | `KEY_INSERT` | 端口菜单（与 R3+L2 等价） |
| 奖杯 | `KEY_F8` | 游戏内奖杯列表（与 R3+R2 等价） |
| 帧率 | `KEY_F9` | 引擎帧率列表（与 R3+L1+R1 等价） |

用键盘键而不是手柄组合，是因为手柄组合（R3+L2 / R3+R2）会**顺带把 L2/R2 按进游戏**
（物品/魔法），而键盘键不会。

### 「怒」为什么是 `MOUSE_MIDDLE_BUTTON`

游戏的斯巴达之怒是 **L3+R3** 同时按。端口默认开着 `rage_one_input`（启动器设置项）：
此时 L3/R3 不再映射成游戏键，**「怒」走独立的一路输入**（默认中键），端口内部再展开成
L3+R3 ⇒ 屏上一个键就能触发。

用中键而不是在屏上凑 L3+R3 两个键：后者会与端口叠加层的 R3 修饰键打架（按住 L2 再按
R3 会被端口当成「开菜单」）。
