# RUNTIME（ARM64 迁移线）

> 本文档描述 `runtime-arm64` 分支——SpartanPort 上游的 ARM64 Runtime 迁移线。
> 上游内容（`main` 分支）不受本分支影响；同步上游 = `git fetch origin` + 按下述模型重放。

## 定位

以 `gow3_windows @ 003e42b`（SpartanPort 2.7.0）为基座，把 PC（x86_64 Linux）实现
迁移为 ARM64 混合原生 runtime：GPU 原生（`libspartanportgpu.so` → 系统 Vulkan 驱动）、
guest x86_64 代码经内集成 FEXCore（`libspartanportcpu.so`）。产品形态为 fexdroid
Runtime 包（`runtime/` 目录产出 `.tar.gz`，契约见 fexdroid 仓
`my-docs/Runtime打包规范.md`）。

## 三层增量模型

| 层 | 内容 | 与上游关系 |
|---|---|---|
| 重放层 | 安卓适配增量（B 类）与仅存于安卓线的缺陷修复（E 类） | 叠加在上游文件上，sync 时先取上游再重放 |
| 新增层 | 上游无对应物的 ARM64 必需文件（`cpu/`、`src/gport_guest_cpu.h`、`src/gport_runtime_cpu.c`、`gpu/shadps4/.../vk_predication.*`、`gpu/shim/gport_pad_input.h`、ARM64 构建链、`third_party/FEX`） | 独立文件，安卓独有符号用 `gport_` 前缀 |
| 打包层 | `runtime/`（runtime.json、entry.sh、pack.py、assets） | 与上游树完全隔离 |

共享文件（上游有对应物者）MUST 与上游字节级一致（重放层文件除外）。
排除清单（不迁入）：音频适配（走系统 PulseAudio）、`run-gport.sh` 启动链
（走 PC 原链 `run.py`/launcher）、Turnip/LSFG/mesa 构建（驱动归 fexdroid 仓库）、
APK/deploy/CI 血统。命名保持上游（`spartanport_*` / `SPARTAN_*`）。

## 来源文档

- 对比清单（A–E 分类与逐文件适配点）：
  `gow_android_glibc/my-docs/05-同步上游/对比上游PC-安卓修改文件清单-20261022.md`
- E.1 配对修复取证：`gow_android_glibc/my-docs/08-Android移植/渲染范围跨代配对-取证与修复-20261023.md`
- Runtime 包规范与首实例记录：fexdroid 仓 `my-docs/Runtime打包规范.md`、
  `my-docs/SpartanPort适配fexdroid启动器-20261024.md`
- 迁移 change：fexdroid 仓 `openspec/changes/migrate-upstream-pc-to-runtime-repo/`
  （proposal / specs / design / tasks）

## 同步上游

1. `git fetch origin && git checkout runtime-arm64 && git merge origin/main`（或 rebase）
2. 归一化比对复算（`tools/check_migration.py`，B 类归一规则）确认 delta 全部落在重放层清单
3. 重放层文件逐个核对：先取上游版本，再重放安卓增量
4. E 类逐条给出"保留并重放 / 显式失效"判定并回填证据
5. 构建验证：ARM64 交叉构建 + 主机单测 + 真机回归（E.1 长时间游玩场景必测）
