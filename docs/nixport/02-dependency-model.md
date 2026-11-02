# 02 · 依赖三层模型 —— 为什么“提供了 sysroot 还缺库”

> 2026-11-02，源自 Rust 启动器（launcher-rust）移植时 `libxkbcommon-x11` 缺失的排查。
> 结论先行：**一个运行库要能被加载，必须过三层；任何一层没兜住就是“缺库”。**

## 三层模型

```
[第1层] 自动依赖打包        build.sh「bundle libraries」
         递归扫 ELF NEEDED → 从 sysroot 抽 .so 进包
         ⚠ 只看链接依赖；dlopen 的运行时依赖看不见
[第2层] sysroot 的包清单    packaging/arm-linux/fetch-sysroot.py 的 WANTED
         sysroot 只含有清单里的包；清单外 = 构建机的“仓库”里没有
[第3层] 宿主排除清单        nixport 仓 scripts/build-port-appimage.sh 的 SOFT_EXCLUDE
         glibc/X11/xcb/xkbcommon/wayland/pulse/fontconfig/vulkan loader…
         = "rootfs 标配，Port 不得携带"（防旧版遮蔽显示桥配套库）
         staging 时按 basename 全 AppDir 扫描剥除 ⇒ 塞进去也会被剥
[第4层·兜底] rootfs payload 必须真的有
         nixport-rootfs 仓 scripts/lib/rootfs-packages.txt
```

## 案例：libxkbcommon-x11.so.0（Rust 启动器/winit 的 X11 后端）

| 层 | 当时状态 | 为什么 |
|---|---|---|
| 1 自动打包 | ❌ 看不见 | winit 用 `dlopen` 按需加载（xkbcommon-dl crate），NEEDED 表没有它 |
| 2 sysroot | ❌ 没有 | WANTED 是游戏运行时的需求清单，没含 `libxkbcommon-x11-0`/`libxcb-xkb1` |
| 3 排除清单 | ❌ 会剥 | `libxkbcommon*`/`libxcb*` 都在 SOFT_EXCLUDE；**改端口侧无效**，扫描是全 AppDir 的 |
| 4 rootfs | ❌ 没有 | 游戏时代拼的 payload；SDL 只链 `libxkbcommon.so.0`，X11 变体从没需要过 |

⇒ 正确落点是**第 4 层**（rootfs）：第 3 层的政策是对的——X11 栈归 rootfs，Port 不带。
修复：nixport-rootfs `rootfs-packages.txt` += `libxkbcommon0` / `libxkbcommon-x11-0` /
`libxcb-xkb1`；sysroot 的 WANTED 同步加（构建期若要引用）。

## 排查口诀

1. **先分类**：`ldd`（或 llvm-readelf -d）看它是 NEEDED 还是 dlopen 隐藏依赖；
   隐藏依赖 ⇒ 自动打包帮不上，要么显式随包（若政策允许），要么归 rootfs。
2. **再查仓**：sysroot 里有没有？`ls .local-deps/arm64-sysroot/usr/lib/aarch64-linux-gnu/`；
   没有 ⇒ fetch-sysroot.py 的 WANTED 加包重拉。
3. **再看政策**：匹配 SOFT_EXCLUDE 吗？匹配 ⇒ 别随包，走 rootfs（改 nixport-rootfs 的清单）。
4. **最后 rootfs**：设备 payload 里有没有？没有 ⇒ 短期 su 手补 + **chcon 对齐标签**（见下），
   长期改 nixport-rootfs 清单刷 payload。

## 设备侧的坑：su 手补 rootfs 库的 SELinux 标签

用 su 放进 rootfs 的文件标签是 `u:object_r:app_data_file:s0`（**无类别**），而 app 进程
带类别（如 `s0:c34,c257,c512,c768`）—— 这台设备的策略要求类别匹配 ⇒ **app 读不到**，
dlopen 报的却是 "library not found"（真正的 EACCES 被吞了）。

修复（对齐同目录里 payload 装的邻居库）：

```bash
HOST=/data/user/0/ro.cobrabm.fexdroid/files/rootfs
adb -s $SER shell "su -c '
  chown u0_a290:u0_a290 $HOST/usr/lib/aarch64-linux-gnu/libXxx.so.1.0.0
  chcon u:object_r:app_data_file:s0:c34,c257,c512,c768 …/libXxx.so.1.0.0
  chown -h/chcon -h …（符号链接本身也要）
'"
```

验证技巧：**别用 su 测**（su 域是 permissive 的，测不出 SELinux 拒绝）——要把探针
（python ctypes.CDLL）放进**会话里跑**（比如临时借 entry.sh 的某个 verb），那才是
app 域的真实视角。另：看 `.last-error.txt`、`ls -Z` 对照邻居、errno 区分 ENOENT/EACCES。

## 本仓库已落地的相关改动

- `fetch-sysroot.py`：WANTED += `libxkbcommon-x11-0`、`libxcb-xkb1`
- `packaging/arm-linux/build.sh`：Rust 启动器构建/装包/依赖遍历；xkb 两库**不随包**（政策）
- `launcher-rust/build-aarch64.sh`：aarch64 交叉构建（rfd 走 xdg-portal 免 GTK）
- nixport-rootfs（`3bc6948`）：rootfs-packages.txt += 三包
- 设备 rootfs：已手补（payload 重刷后自带，可删手补）
