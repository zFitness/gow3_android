# nixport 专栏 —— ARM64 移植线的实战笔记

本专栏是**本分支（ARM64 Android 移植线）自己的**文档，不进上游文档体系。
记录排查技巧、事故复盘、设备侧的坑，供以后排查同类问题时直接翻。

| 篇 | 内容 |
|---|---|
| [01 排查手册](01-debug-playbook.md) | 日志在哪、怎么抓现场、A/B 开关、事故模式速查表 |
| [2026-11-01 未授权发布 release](../incidents/2026-11-01-未授权发布-release.md) | 事故复盘 + 发布规则 R1–R6（也见 [发布策略](../port/release-policy.md)） |

## 一页速用（赶时间只看这段）

```bash
# 1) 游戏输出（崩溃现场第一现场；这台设备 shell 可直读 app 私有目录，su 也可用）
adb -s HA2EFNGR shell "tail -80 /data/user/0/ro.cobrabm.fexdroid/files/imports/spartanport/home/gport/user/last_run.log"

# 2) 宿主视角：帧计数冻结 = guest 停止出帧；perf = guest 各线程 5 秒采样
cd ../fexdroid && python3 tools/fxctl/fxctl.py --serial HA2EFNGR status   # counters
python3 tools/fxctl/fxctl.py --serial HA2EFNGR perf

# 3) 崩溃模式速判：所有游戏线程集体卡 4–6 秒 → GPU ErrorDeviceLost → 退出码 23
#    = GPU 侧（Turnip）出事；不是 guest 逻辑、不是我们的 C++ 崩。

# 4) A/B 一个 GPU 开关（改前先停会话，改完恢复属主）
adb -s HA2EFNGR shell "su -c \"sed -i 's/^deferred_readback=1\$/deferred_readback=0/' \
  /data/user/0/ro.cobrabm.fexdroid/files/imports/spartanport/home/gport/spartanport.ini\""
```
