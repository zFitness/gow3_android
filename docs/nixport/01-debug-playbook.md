# 01 · 排查手册 —— nixport 端口的 debug 技巧

> 2026-11-01 由 3.0.0 黑屏/闪退排查整理。所有路径以设备 TB322FC（拯救者 Y700 平板，
> Android 16，Adreno 830）为准；其它设备同构，只是序列号不同。

## 1. 日志与配置都在哪

| 文件 | 内容 | 谁写 |
|---|---|---|
| `files/imports/spartanport/home/gport/user/last_run.log` | **游戏输出**（probe 的 stdout/stderr 全量，崩溃现场在这里） | 启动器的运行链（`--play` 与游戏内重启都写它） |
| `files/imports/spartanport/home/gport/logs/launch.log` | entry.sh 的 `game` verb 链路日志 | entry.sh（宿主「运行」直达游戏时才产生；启动器链不写它） |
| `files/imports/spartanport/home/gport/spartanport.ini` | 运行配置（分辨率/读回/锐化/作弊…） | 启动器写入、游戏内菜单改完退出时写回 |
| `files/imports/spartanport/install.json` | 宿主的安装记录（manifest 折叠而来） | 宿主导入时写 |
| 宿主会话日志 | 宿主自己的启动/桥/输入日志（`[port-spartanport]` 前缀只有 fexpad 的行） | 宿主（fxctl log 可读） |
| logcat | 内核/系统侧（kgsl GPU fault 等会被环形缓冲冲掉，要抓趁早） | 系统 |

这台设备 **adb shell 直读 app 私有目录可行，`su` 也可用** ⇒ 抓日志不用绕宿主 UI：

```bash
SER=HA2EFNGR
P=/data/user/0/ro.cobrabm.fexdroid/files/imports/spartanport/home/gport
adb -s $SER shell "tail -80 $P/user/last_run.log"          # 现场尾部
adb -s $SER pull  $P/user/last_run.log /tmp/lastrun.log    # 全量拉回
```

## 2. 启动器面板 = 日志的图形视图

端口启动器（Tk）「**高级 → 日志**」就是 `last_run.log` 的视图（有复制/清除按钮）。
崩溃后不需要 adb 也能在屏幕上看到栈；面板只滚到尾部，要看头部用上面的 adb 拉。

## 3. 宿主「导出日志」的边界

宿主「详情 → 导出日志 / 分享日志」**只包含 `port.json` 的 `logs:` 数组里声明的文件**
（外加宿主设备事实、自检、崩溃快照）。要让 `last_run.log` 能导出，必须在
`packaging/arm-linux/port.json` 的 `logs:` 里声明（diag 包即这么做）并重新导入。
注意：**改 manifest 后必须重新导入包**，宿主的安装记录（install.json）才更新。

## 4. fxctl：宿主视角的探针（fexdroid 仓 `tools/fxctl/`）

```bash
cd ../fexdroid && python3 tools/fxctl/fxctl.py --serial HA2EFNGR <命令>
```

| 命令 | 用途 |
|---|---|
| `status` | 会话状态 + **帧计数**（`counters.frames` 停住 = guest 停止出帧） |
| `counters` | 只看帧计数（隔几秒采样两次对比） |
| `perf` | 5 秒采样：guest 各**线程** CPU 占比（自旋 vs 全阻塞一眼分辨）、GPU 忙碌、音频空洞 |
| `log -n N` | 宿主会话日志（含 `[port-spartanport]` 前缀的 fexpad 行） |
| `report` | 设备事实 + 3 秒采样 + 会话日志 + 自检（大而全，适合贴给他人） |
| `stop` | 结束会话（卡死时用；日志文件不受影响） |

## 5. SPARTAN_SLOW_WAITS：卡死时它点名线程

`entry.sh` 默认 `SPARTAN_SLOW_WAITS=2000`（上游 `runtime_thread.c` 的诊断）：
guest 的等待超过阈值就打印一行：

```
Slow wait: cond wait 3794.9 ms, thread 'SceFiosIO' cond 64424516160
Slow wait: semaphore wait 3786.1 ms, thread 'WorkerThread1' id 1
Slow wait: equeue wait 6547.3 ms, thread 'eop_thread' done 1
Slow wait: file read 2100.3 ms, thread '...' bytes 1048576
Slow wait: mutex lock 2500.0 ms, thread '...' mutex 4397019136
```

**读法**：哪类等待、哪个 guest 线程、卡了多久。
- `cond/semaphore/equeue` 全线同时卡 4–6 秒又恢复 ⇒ 系统级停顿（GPU/驱动或宿主卡顿）；
- 只有一个线程长期卡 ⇒ 那条路径自己的锁/死锁；
- `file read/pread` 卡 ⇒ 存储慢（sdcard/FUSE）。

## 6. 事故模式速查表

| 症状 | 日志特征 | 结论 |
|---|---|---|
| 黑屏但进程活着，帧计数冻结 | 无 STOP，可能有一波 Slow wait | GPU 侧卡住的前兆；等它走到下一行 |
| 闪退，退出码 **23**（实例：deferred_readback=1，Adreno 830，已修） | `GPU <Critical> vk_semaphore/vk_scheduler … ErrorDeviceLost / Device lost during submit` + 前面一波全线程 Slow wait | **Vulkan 设备丢失**（Turnip/GPU 出事），不是 guest 逻辑、不是我们的 C++ 崩 |
| 闪退，栈里有 `libvulkan_freedreno.so` | backtrace 顶部是驱动帧 | 驱动内崩：我们提交了驱动不认的东西（同步/图像状态），或驱动 bug |
| `STOP: watchdog timeout; thread stacks:` | probe 的 alarm 看门狗（默认启动后 10 秒一次性） | 启动期卡死；运行期不重挂 |
| 读档/存档点附近卡 | `SaveDataDialog…` / `AutoSaveHDD` / `SceFiosIO` Slow wait | 存档 I/O 路径；对照关自动存档试试 |

## 7. A/B 开关（改 `spartanport.ini`，一次只改一个）

| 键 | 默认 | 关掉验证什么 |
|---|---|---|
| `deferred_readback` | 上游 1 / **设备线 0** | 延迟回读（GPU 日志会打 `deferred image readback enabled`）。**已确认**：Adreno 830 的 Turnip 上开着它会 Vulkan 设备丢失（2026-11-01 两次事故：黑屏帧冻结 / 退出码 23 的 ErrorDeviceLost；关掉后长玩未复现），设备线默认已改 0 |
| `stale_readback` | 1 | 旧帧回读（CPU 读上一帧拷贝） |
| `async_shaders` | 1 | 异步着色/绘制准备（`draw preparation workers: N`） |
| `readback_linear` | 1 | 端口自己的读回线性化（emulator flag `SPARTAN_READBACK_LINEAR`） |
| `rcas` | 上游 1 | 3.0.0 的每帧锐化 pass（注意：分辨率不匹配窗口时该 pass 实际不跑，先算 `FsrPassRuns`） |
| `fsr1` | 0 | FSR 1 放大 |

改法（**先停会话**——游戏退出时会写回 ini，改了白改；改完恢复属主，别让 app 写不回）：

```bash
INI=/data/user/0/ro.cobrabm.fexdroid/files/imports/spartanport/home/gport/spartanport.ini
adb -s HA2EFNGR shell "su -c \"sed -i 's/^deferred_readback=1\$/deferred_readback=0/' $INI\"
                        && su -c \"chown u0_a289:u0_a289 $INI && chmod 600 $INI\""
```

游戏内菜单（Insert / R3+L2 → 图形）也能**实时**开关一部分（RCAS、Bloom、Motion Blur、
景深、Texture Quality…），改动退出时写回 ini —— 适合快速试，但确认结论要以 ini 为准。

另两个便宜动作：
- **清着色器缓存**（启动器 高级 → 清除着色器缓存）：黑屏/花屏第一步；
- **关自动存档**对照：卡死集中在 `AutoSaveHDD` 触发点时。

## 8. 确认设备上装的是哪个包

三种办法，从快到慢：
1. 宿主「详情」页的**大小**（各构建略有差异）；
2. `adb shell cat …/imports/spartanport/install.json` 看 `logs`/`version`（导入期写入的记录）；
3. 宿主「选项」页看新增的开关（每加一批选项就是一个新构建的指纹）。

## 9. 换包测试的标准流程

```bash
cd gow3_windows && bash packaging/arm-linux/build.sh        # 产物 dist/spartanport-port.zip
adb -s HA2EFNGR push packaging/arm-linux/dist/spartanport-port.zip \
    /sdcard/Download/SpartanPort.nixport.<版本>-<用途>.zip   # 新文件名，不覆盖旧包
```
宿主「+ → 导入 Port 分发包」选中它（同 id 覆盖安装；sdcard 上的旧 zip 不受影响）。

## 10. 已知坑

- **改 ini 前先停会话**：游戏退出时把内存里的设置写回 ini，会话中进行修改会被覆盖。
- **文件属主**：su 改完记得 `chown u0_a289:u0_a289` + `chmod 600`，否则 app 写不回。
- **install.json 不随 ini 变**：它只在导入时写；诊断导出的内容以它为准。
- **logcat 的内核 GPU fault**（kgsl/adreno）会被环形缓冲冲掉，崩溃后要尽快抓：
  `adb logcat -d -b kernel | grep -iE "kgsl|adreno|fault"`。
- **fxctl 的 `run_as` 不可用**（release 包 not debuggable）⇒ 文件操作走 shell/su 直读。
