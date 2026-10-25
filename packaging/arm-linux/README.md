# packaging/arm-linux — ARM64 Linux 构建与 fexdroid Runtime 打包

把 SpartanPort 交叉构建为 **ARM64 Linux 原生程序**（GPU 原生 + guest x86_64 经 FEXCore），
并组装成 fexdroid Runtime 包（`.tar.gz`，契约见 fexdroid 仓 `my-docs/Runtime打包规范.md`）。

与 `packaging/linux/`（x86_64 AppImage）同构：同一套构建步骤的交叉变体，程序树采用上游
packaged 布局（`bin/spartanport-probe`、`bin/gpu/`、`run.py`、`scripts/`），差异只有三点：

1. 工具链 = clang `--target=aarch64-linux-gnu` + Debian arm64 sysroot（`fetch-sysroot.py` 产出）
2. 跳过 x86 宿主专属源（`isa_emulate/cpu_check/isa_rewrite`——guest x86 代码由 FEXCore 执行）
3. probe 链接时烤入 fexdroid rootfs 的 ELF 解释器（Android 沙箱没有 `/lib`）

## 用法

```bash
bash packaging/arm-linux/build.sh      # 构建 + 组包 + verb 冒烟 → dist/spartanport-runtime.tar.gz
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
