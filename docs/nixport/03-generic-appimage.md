# 03 · 通用 AppImage（普通 ARM Linux）的坑与排查实录

> 2026-11-02，源自通用 ARM64 AppImage 在 KDE + Ubuntu (ARM64) 环境的首轮实测。
> 两种变体的定位差异见 [README](README.md)：**nixport Port 包**面向 nixport 宿主
> （烤入解释器、依赖 rootfs）；**通用 AppImage** 面向普通 aarch64 Linux 桌面
> （标准 `/lib` 解释器、依赖自包含）。构建开关：`SPARTAN_GENERIC_APPIMAGE=1`。

## 1. 点击 Browser（游戏目录选择）无反应 —— 三层原因

### 第 1 层：rfd 的 Linux 后端 = xdg-portal，需要 portal 服务

- 上游 rfd 0.17 的 Linux 默认后端是 **GTK3**；我们的 ARM64 交叉编译 sysroot 没有
  GTK3 的 C 库 ⇒ 后端换成 **xdg-portal**（纯 Rust，走 DBus 的
  `org.freedesktop.portal.Desktop` FileChooser）。
- 该后端要求目标环境**运行 xdg-desktop-portal 服务**（KDE/GNOME 桌面默认有；
  最小安装/容器经常没有）。
- rfd 对 portal 失败是**静默的**（返回 None，错误被吞）⇒ 点击无任何反应。

**诊断**（在该环境的终端里）：

```bash
systemctl --user status xdg-desktop-portal xdg-desktop-portal-kde
busctl --user tree org.freedesktop.portal.Desktop | head
```

服务没跑 ⇒ `sudo apt install xdg-desktop-portal xdg-desktop-portal-kde`（KDE）。

### 第 2 层：数据目录不可写 —— "部分设置保存失败"

通用 AppImage 的 AppRun 若不设 `SPARTAN_DATA_DIR`，Rust 启动器的数据目录
默认落在**exe 旁边 = AppImage 的只读 squashfs 挂载点** ⇒ settings.json / ini /
patches.json 全部写失败 ⇒ 报"部分设置保存失败，请检查写入权限"。

**修复**（已进 build.sh 的 generic 阶段）：AppRun 对齐上游 Linux AppImage：

```sh
export SPARTAN_DATA_DIR=${SPARTAN_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/spartanport}
export SPARTAN_USER_DIR=${SPARTAN_USER_DIR:-$SPARTAN_DATA_DIR/user}
mkdir -p "$SPARTAN_DATA_DIR" "$SPARTAN_USER_DIR"
```

### 第 3 层：键表数据缺失 —— XKBNotFound

`/usr/share/X11/xkb`（xkb-data 包）不在极简系统里时，winit 起事件循环报
`XKBNotFound`。**通用 AppImage 自带键表**（AppRun 设 `XKB_CONFIG_ROOT` 指
AppImage 内的副本）⇒ 极简系统也能跑。

## 2. 部署排查技巧：确认"正在跑的进程"加载的是哪个二进制

替换设备上的二进制后，**正在运行的进程还是旧代码**（内存里的旧映像）。
核实方法：

```bash
PID=$(pgrep -f 'spartanport-launcher-rus[t]' | head -1)   # [t] 字符类防自杀
md5sum /proc/$PID/exe                                     # 对比本机构建的 md5
```

- 不一致 ⇒ 进程还是旧的：让用户退出会话重开（或按 PID kill）后再测。

**pkill 自匹配坑**：`su -c 'pkill -f "xxx" ; cp … /xxx/yyy'` 这种链，命令行里
出现的**目标路径字符串**（含 xxx）会被 pkill 自己的模式匹配到 ⇒ 把自己的
shell 杀掉 ⇒ 整条链静默死亡（无任何输出）。规避：用 `[t]` 字符类技巧、或把
kill 与 cp 拆成**两次独立的 adb 调用**。

## 3. 图形三开关（FEX 钩子）的复盘 —— 已回退

游戏内 Options > Graphics 的 Shadows / Motion Blur / Bloom 开关：上游靠往
guest 写跳板实现"跳过 pass"；FEX 陷阱模型下移植后（`install_graphics_fex`，
钩子安装成功 ✓）**切换动作本身导致渲染挂死黑屏**。

结论：FEX 陷阱钩子适合**只读/旁路型**功能（金手指读值、数值倍率、敌血条
记录、QTE 自动作答——已验证稳定），**不适合控制 pass 执行与否**（切到
"跳过"会让渲染管线进入挂死状态）。ARM64 上这三个 pass 保持常开（与上游
PC 默认一致）。SSAO / 软粒子由静态补丁关闭（无开关替代，移动端省性能）。

若未来要重试：改走 **hook slot** 路线（AOT 分支的编译期插槽数据），或先在
native x86 上确认 stub 的跳过语义在 CUSA01740 上无副作用。

## 4. rootfs 三包（libxkbcommon-x11 / libxcb-xkb / xkb-data）

- rootfs payload 构建时代缺失 ⇒ Rust 启动器起不来（见第 1 层/2 层的两类 panic）。
- **持久修复**：nixport-rootfs 仓 `scripts/lib/rootfs-packages.txt` 加了
  `libxkbcommon0 / libxkbcommon-x11-0 / libxcb-xkb1 / xkb-data`（`3bc6948`+`ce0fe1b`）。
- **payload 发布已含三包**（built_at 2026-11-02T09:30Z，built_from e79e228）⇒
  payload 重刷/换设备不再丢失。
- **su 手补的 SELinux 坑**：su 建的文件标签是 `app_data_file:s0`（无类别），
  app 进程（带 `c34,c257,...` 类别）读不到，dlopen 报的却是 "not found"
  （真正的 EACCES 被吞）⇒ 必须 `chown u0_a290:u0_a290` + `chcon` 对齐邻居库
  的标签。**验证要放会话里跑**（su 域 permissive，测不出 SELinux 拒绝）。

## 5. ini 被 Rust 启动器重写时丢未知键

Rust 启动器退出/保存时按**它认识的键集**重写 `spartanport.ini` ⇒ 手工加的
自定义键（如 `launcher=rust`）会被丢掉。排查 ini "改了没生效"时先确认：
改完之后启动器是否又运行/退出过（写回覆盖）。

## 6. 双变体的构建要点（build.sh generic 阶段）

- probe/启动器**重链接为标准 `/lib` 解释器**（不烤 nixport rootfs 路径）：
  probe 去掉 `-Wl,--dynamic-linker`；启动器 `SPARTAN_BAKE_INTERPRETER=0`。
- 依赖闭包：`bundle()` 把 NEEDED 递归闭包装进 `$out/lib`（排除清单外的全带），
  generic AppDir **显式 `cp -R $out/lib → usr/lib`**——踩过的坑：只调 bundle
  不拷贝 ⇒ AppDir 里 lib 为空 ⇒ 目标机缺 SDL3/ffmpeg 直接起不来。
- 键表数据：本地缓存 `.local-deps/xkb-data/`（缺失时构建期从 Debian 镜像拉
  `xkb-data_2.42-1_all.deb` 解出）。
- AppImage 组装（宿主组包器）对 generic AppDir 同样适用：AppDir 布局对齐
  上游（`usr/share/spartanport/` + `usr/lib/` + AppRun），其排除清单会剥掉
  X11/xcb/xkb 库 ⇒ 通用 AppImage 依赖目标系统的 X11 栈（桌面发行版标配）。
- glibc 天花板 = 构建 sysroot 的版本（trixie = 2.41）：目标机 glibc 低于它
  ⇒ 无法运行（上游 x86 AppImage 用 Ubuntu 24.04 sysroot 是同一个道理）。
