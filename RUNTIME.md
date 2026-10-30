# RUNTIME（ARM64 迁移线）

> 本文档描述 `runtime-arm64` 分支——SpartanPort 上游的 ARM64 Runtime 迁移线。
> 上游内容（`main` 分支）不受本分支影响；同步上游 = `git fetch origin` + 按下述模型重放。

## 定位

以 `gow3_windows @ 003e42b`（SpartanPort 2.7.0）为基座，把 PC（x86_64 Linux）实现
迁移为 ARM64 混合原生 runtime：GPU 原生（`libspartanportgpu.so` → 系统 Vulkan 驱动）、
guest x86_64 代码经内集成 FEXCore（`libspartanportcpu.so`）。产品形态为 fexdroid
Port 分发包（`packaging/arm-linux/dist/` 产出 `.zip`：包根恰含 `port.json` 与恰好一个
`.AppImage`，契约见 fexdroid 仓 `my-docs/移植规范/Port打包规范.md`）。

## 三层增量模型

| 层 | 内容 | 与上游关系 |
|---|---|---|
| 重放层 | 安卓适配增量（B 类）与仅存于安卓线的缺陷修复（E 类） | 叠加在上游文件上，sync 时先取上游再重放 |
| 新增层 | 上游无对应物的 ARM64 必需文件（`cpu/`、`src/gport_guest_cpu.h`、`src/gport_runtime_cpu.c`、`gpu/shadps4/.../vk_predication.*`、`gpu/shim/entry 契约`、ARM64 构建链、`third_party/FEX`） | 独立文件，安卓独有符号用 `gport_` 前缀 |
| 打包层 | `runtime/`（runtime.json、entry.sh、pack.py、assets） | 与上游树完全隔离 |

共享文件（上游有对应物者）MUST 与上游字节级一致（重放层文件除外）。
排除清单（不迁入）：音频适配（走系统 PulseAudio）、`run-gport.sh` 启动链
（走 PC 原链 `run.py`/launcher）、Turnip/LSFG/mesa 构建（驱动归 fexdroid 仓库）、
APK/deploy/CI 血统。命名保持上游（`spartanport_*` / `SPARTAN_*`）。

## 设备线增量登记

上游文件上叠加的**设备线（安卓）增量**：文件内容 = 上游版本 + 下列增量，
与上游永久不同。同步上游时按本表重放，不要整文件取上游。

| 文件 | 上游锚点 | 增量 | 依据 |
|---|---|---|---|
| `launcher/spartanport_launcher_win.py` | `size_window()` | 设备会话（`FEXDROID_PRESENT` 存在）时窗口铺满屏幕 `+0+0`；桌面路径逐字节不变 | 客机侧 `gpu/shim/window.cpp` 的 `PresentBridgeActive()` 用同一判据让游戏窗口铺满整屏；启动器必须同几何，否则设备上表现为"启动器靠左"、PLAY 前后缩放跳动 |
| `gpu/shim/spartanport_overlay.cpp` | `Render()` 的 UI 缩放 | `std::max(extent.height / 1080.0f, 0.75f)` → 下限 `1.0f` | 设备虚拟屏常为 720 高，`720/1080 = 0.667` 落到 0.75 下限会把基准 18px 字号缩到 13.5px，菜单偏小 |
| `gpu/CMakeLists.txt` | 目标平台开关 | 新增 `option(SPARTAN_ARM)` / `option(SPARTAN_NIXPORT)`（默认 OFF）与其 `add_compile_definitions`；删掉 `pad-input-test` 目标 | ARM Linux 与安卓共用一份二进制，GPU 代码按目标平台分支；nixport 侧走宿主服务总线与移动端样式。桌面构建两个开关都关，行为与上游一致。`tests/test_pad_input.cpp` 随 pad 文件桥退役不迁入（SKIP 清单），目标留着 cmake configure 就找不到源文件 |
| `gpu/shim/spartanport_settings.h` | `SpartanSettings::Values` 的缺省值 | 缺省值改用 `SPARTAN_DEVICE_*` 宏：ARM 构建下关升采样与物体运动抖动、限帧 60、渲染 720p、引擎帧率 60；NIXPORT 构建下覆盖层字号 150% | 移动端 GPU 预算有限，桌面缺省在手机上跑不动；游戏画面缩到手机屏幕后屏幕内菜单偏小。与 `scripts/graphics_options.py` 的 `DEVICE_DEFAULTS` 同源，改一处要同时改另一处 |
| `gpu/shim/spartanport_graphics.h` | `Resolutions` / `ResolutionSizes` | ARM 构建下追加 `540p`（960×540）与 `1080p`（1920×1080）两个名字与尺寸（数组长度随 `SPARTAN_EXTRA_RESOLUTION_COUNT` 变） | 安卓侧补丁数据集带这两个分辨率（`patches/*.xml` 的 `Resolution Patch - 540p` / `- 1080p`，上游没有）；客机不认这两个名字，ini 里的 `render_resolution=540p` 会解析成 native 并被写回 native。桌面构建不带它们 |
| `gpu/shim/spartanport_ui_font.h` | `FindChineseFont()` | 候选表前加宿主铺进 rootfs 的 CJK 字体：`$FXD_ROOT/usr/share/fonts/truetype/fexdroid/NotoSansCJKsc-Regular.otf`（同相对路径的绝对路径也试） | 宿主按 guest-fonts 规范把 Noto Sans CJK SC 铺进每棵文件树，但原生会话的 `/` 是安卓的，绝对路径看不见 → 找不到字体，游戏内菜单回落成英文。`FXD_ROOT` 由宿主会话注入 |
| `src/probe.c` | 上游的 `install_qte_hooks` / `install_auto_qte_hooks` / `find_engine_fps` / `publish_engine_fps` / `install_menu_exit` | 补上这五个上游函数：原生 x86-64 路径原样回放（镜像里写 `0xe9` 跳板），FEX JIT 路径另做一套（位点换成 2 字节 syscall 陷阱 + rip 重定向到 trampoline stub，与金手指同款机制） | 上游 2.7.0 新增的 QTE 提示与 Automatic QTE（提交 `dbba963` / `41e9473`）在安卓参考仓里没有；参考仓只有 `runtime_pad.c` 的一半（读提示类型但没人装钩子）→ 菜单里「Automatic QTE」恒为置灰、"引擎帧率"是死的。位点签名在设备 ROM 上全部唯一命中，是 runtime 线缺功能而非 ROM 不支持。详见迁移专栏 13 篇（2026-10-30 真机已验证：六个位点全装上）；同轮把暂停菜单的 "Exit to Desktop" 也补上了（`--pause-menu` 从"接受并忽略"改为存值 + 装钩子，FEX 侧走陷阱路径，stub 与 provider vtable 都用 guest 可调用的蹦床；详见迁移专栏 10 篇的实施结果） |

识别方式：代码里带 `# gport:` / `// gport:` 标记（仓库既有约定），
`tools/check_migration.py` 的 `PARTIAL` 集合登记了这些文件（状态 `PARTIAL`
= "有意只重放部分增量、与参考仓永久不同"）。重放时只取上游对应的
那几行，不要覆盖整文件。

## 目标平台开关（编译期 / 构建期）

| 开关 | 含义 | C++ 侧 | Python 侧 |
|---|---|---|---|
| `SPARTAN_ARM` | 目标是 ARM：ARM Linux 与安卓共用同一份二进制，GPU 代码按目标架构分支 | `gpu/CMakeLists.txt` 的 `option()` → `add_compile_definitions` | `scripts/build_flags.py` 的 `ARM` |
| `SPARTAN_NIXPORT` | 目标是 nixport 平台：宿主服务总线（toast / dialog / 选择屏）与移动端样式 | 同上 | `scripts/build_flags.py` 的 `NIXPORT` |

两个开关**默认 OFF**，只有 nixport 构建线（`packaging/arm-linux/build.sh`）显式打开；
桌面构建两个都关 → 行为与上游逐字节一致。

Python 没有编译期，所以构建脚本把同一组值写进程序树的
`config/gport-build-flags.json`（`scripts/build_flags.py` 读它）：**C++ 的 `-D` 与 Python 的
标记文件同源于 `build.sh` 里的 `spartan_arm` / `spartan_nixport` 两个变量**，不会漂移。
程序树里没有这个文件 = 桌面构建 = 两个开关都关。

- `SPARTAN_ARM` 的消费者：`gpu/shim/spartanport_settings.h`（设备缺省值）、
  `gpu/shim/spartanport_graphics.h`（设备线的 540p / 1080p 分辨率）、
  `scripts/graphics_options.py`（`DEVICE_DEFAULTS` + `DEVICE_RESOLUTIONS`）、`scripts/patches.py`
  （去特效补丁默认勾选）、`launcher/spartanport_launcher_win.py`（`INI_DEFAULTS` 与补丁初值）。
- `SPARTAN_NIXPORT` 的消费者：`scripts/nixport_host.py`（宿主选择屏）、
  `gpu/shim/spartanport_settings.h`（覆盖层字号）。

## 构建（ARM64）

```bash
bash packaging/arm-linux/build.sh     # 交叉构建 + 组包 + verb 冒烟 → dist/spartanport-port.zip
bash packaging/arm-linux/deploy.sh    # adb 推送到 fexdroid debug app（真机调试）
```

流程与根 `build.sh` Linux 分支同构的交叉变体（差异三点：交叉工具链/sysroot、跳过 x86 宿主
专属 ISA 源、烤入 rootfs ELF 解释器），详见 `packaging/arm-linux/README.md`。

## 来源文档

- 对比清单（A–E 分类与逐文件适配点）：
  `gow_android_glibc/my-docs/05-同步上游/对比上游PC-安卓修改文件清单-20261022.md`
- E.1 配对修复取证：`gow_android_glibc/my-docs/08-Android移植/渲染范围跨代配对-取证与修复-20261023.md`
- Runtime 包规范与首实例记录：fexdroid 仓 `my-docs/Runtime打包规范.md`、
  `my-docs/SpartanPort适配fexdroid启动器-20261024.md`
- 迁移 change：fexdroid 仓 `openspec/changes/migrate-upstream-pc-to-runtime-repo/`
  （proposal / specs / design / tasks）
- 迁移专栏（逐篇：真机实录 / 排查实录 / 专题）：
  `openspec-hub/fexdroid/my-docs/移植案例/SpartanPort迁移专栏/`
  （本轮 QTE 与 Automatic QTE 见 `13-QTE提示与Automatic-QTE.md`）

## 同步上游

1. `git fetch origin && git checkout runtime-arm64 && git merge origin/main`（或 rebase）
2. 归一化比对复算（`tools/check_migration.py`，B 类归一规则）确认 delta 全部落在重放层清单
3. 重放层文件逐个核对：先取上游版本，再重放安卓增量
4. E 类逐条给出"保留并重放 / 显式失效"判定并回填证据
5. 设备线增量按上面的"设备线增量登记"表逐条重放（`PARTIAL` 集合里的文件）
6. 构建验证：ARM64 交叉构建 + 主机单测 + 真机回归（E.1 长时间游玩场景必测）
