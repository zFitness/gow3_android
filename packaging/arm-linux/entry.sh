#!/system/bin/sh
# fexdroid Runtime 包唯一契约面（verb 协议，见 my-docs/Runtime打包规范.md §5）。
# stdout 必须是纯 JSON 的是 config/info verb；run 的输出进会话日志（诊断走 stderr）。
#
# 启动链本体在 run.py（上游脚本，POSIX 已修）；本脚本只负责：
#   1. fexdroid 宿主环境块（X11/PulseAudio/显示桥/pad 文件桥/布局变量派生——run.py 不管这些）
#   2. config/info 契约（fexdroid 卡片的表单与只读列表）
# 布局自治：一切路径从本脚本位置派生，fexdroid 注入的 env 只作覆盖。
set -eu

TREE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)   # 程序树根（<包根>/<program>）
PKG=$(dirname "$TREE")                               # 包根
DATA="${SPARTAN_DATA_DIR:-$PKG/gdata}"
GHOME="${SPARTAN_HOME:-$PKG/home/gport}"
CONFIG="${SPARTAN_CONFIG:-$DATA/spartanport.ini}"
OUT="$DATA/out"
GAME_DIR="${SPARTAN_GAME_DIR:-}"
HOST="${FXD_ROOT:-/data/user/0/ro.cobrabm.fexdroid/files/rootfs}"
PAD="${SPARTAN_PAD_FILE:-${FXD_FILES:+$FXD_FILES/android-pad.state}}"
PY=python3.13; command -v "$PY" >/dev/null 2>&1 || PY=python3
PROBE="$TREE/bin/spartanport-probe"
# python 统一走显式 loader（烤入的解释器路径在跨 app/无会话场景不可靠；旧链同款模式）
LOADER="$HOST/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
py_exec() {
    if [ -x "$LOADER" ]; then exec "$LOADER" "$PY" "$@"; else exec "$PY" "$@"; fi
}

launch_env() {
    mkdir -p "$DATA/user" "$DATA/logs" "$OUT" "$GHOME"
    [ -f "$CONFIG" ] || : > "$CONFIG"
    export SPARTAN_PORT_DIR="$TREE" SPARTAN_DATA_DIR="$DATA" SPARTAN_HOME="$GHOME"
    export SPARTAN_CONFIG="$CONFIG" SPARTAN_USER_DIR="$DATA/user" SPARTAN_LOG_DIR="$DATA/logs"
    [ -n "$PAD" ] && export SPARTAN_PAD_FILE="$PAD"
    # guest glibc：桌面同款 hwcaps 关闭（FEXCore 下不受支持指令）
    export GLIBC_TUNABLES='glibc.cpu.hwcaps=-AVX2,-AVX,-AVX_Fast_Unaligned_Load,-AVX2_Usable,-AVX_Usable'
    export SDL_VIDEODRIVER=x11 DISPLAY=:0
    export MESA_VK_WSI_DEBUG=sw MESA_VK_WSI_PRESENT_MODE=relaxed
    export FEXDROID_PRESENT="${FEXDROID_PRESENT:-$HOST/tmp/fxpresent.sock}"
    export SDL_JOYSTICK_HIDAPI=0
    [ -f "$TREE/config/gport-gamecontrollerdb.txt" ] &&
        export SDL_GAMECONTROLLERCONFIG_FILE="$TREE/config/gport-gamecontrollerdb.txt"
    export PULSE_SERVER="${PULSE_SERVER:-unix:$HOST/tmp/pulse/native}" PULSE_LATENCY_MSEC=60
    # 打包库（rootfs 缺的 SDL3/ffmpeg 等）；bin/gpu 由 probe 的 RUNPATH 自取
    export LD_LIBRARY_PATH="$TREE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export PATH="$TREE/bin:$PATH"
}

verb="${1:-}"; [ $# -gt 0 ] && shift || true

case "$verb" in
game|run)
    launch_env
    [ -n "$GAME_DIR" ] || { echo "Runtime: SPARTAN_GAME_DIR is not set" >&2; exit 2; }
    export SPARTAN_PROBE="$PROBE"
    cd "$TREE"
    py_exec run.py "$@"
    ;;
launcher)
    launch_env
    if ! "$PY" -c 'import tkinter' 2>/dev/null; then
        printf '{"error":"tkinter missing: install python3-tk into the rootfs (fexdroid scripts/add-python313-tk.sh)"}\n'
        exit 3
    fi
    py_exec "$TREE/launcher/spartanport_launcher_win.py"
    ;;
check|vulkan)
    launch_env
    if [ "$verb" = vulkan ]; then
        exec "$PROBE" --vulkan-only
    fi
    [ -f "$OUT/boot-linked.bin" ] || { echo "Runtime: prepare first (run game once)" >&2; exit 2; }
    exec "$PROBE" "$OUT/boot-linked.bin" --app0 "${GAME_DIR:-$PKG/game}" --check-imports
    ;;
config)
    sub="${1:-}"; [ $# -gt 0 ] && shift || true
    [ -d "$DATA" ] || mkdir -p "$DATA"
    [ -f "$CONFIG" ] || : > "$CONFIG"
    py_exec - "$sub" "$CONFIG" "$@" <<'PYEOF'
import json, sys, shutil

SCHEMA = {
  "groups": [
    {"id": "cheats", "title": "作弊码", "advanced": False, "items": [
      {"key": "cheat_health", "title": "无限生命", "type": "boolean", "default": False},
      {"key": "cheat_magic", "title": "无限魔法", "type": "boolean", "default": False},
      {"key": "cheat_item", "title": "无限物品", "type": "boolean", "default": False},
      {"key": "cheat_rage", "title": "无限怒气", "type": "boolean", "default": False},
      {"key": "cheat_orbs", "title": "无限红魂", "type": "boolean", "default": False},
      {"key": "cheat_very_hard", "title": "解锁混沌难度", "type": "boolean", "default": False},
      {"key": "cheat_double_jumps", "title": "二段跳", "type": "boolean", "default": False},
      {"key": "auto_qte", "title": "QTE 自动", "type": "boolean", "default": False},
      {"key": "enemy_health_bar", "title": "敌人血条", "type": "boolean", "default": True}]},
    {"id": "video", "title": "画面", "items": [
      {"key": "render_resolution", "title": "渲染分辨率", "type": "enum",
       "enum": ["native", "480p", "720p", "900p", "1440p", "1800p", "4K"],
       "default": "native", "restart": True},
      {"key": "engine_fps", "title": "目标帧率", "type": "enum",
       "enum": ["60", "100", "120", "144", "180", "240"],
       "default": "120", "restart": True},
      {"key": "upscaler", "title": "超分辨率", "type": "enum",
       "enum": ["off", "fsr3", "fsr4", "fsr411", "taa", "dlss"],
       "default": "off", "restart": True},
      {"key": "sharpness", "title": "锐化", "type": "number", "min": 0, "max": 2, "step": 0.05, "default": 0.8},
      {"key": "anisotropy", "title": "各向异性过滤", "type": "enum",
       "enum": ["0", "2", "4", "8", "16"], "default": "0", "restart": True},
      {"key": "vsync", "title": "垂直同步", "type": "boolean", "default": False, "restart": True},
      {"key": "fps_limit", "title": "帧率上限", "type": "enum",
       "enum": ["0", "30", "60", "100", "120", "144", "180", "240"],
       "default": "0", "note": "0 = 不限制"}]},
    {"id": "overlay", "title": "性能悬浮窗", "advanced": True, "items": [
      {"key": "show_fps", "title": "显示帧率", "type": "boolean", "default": False},
      {"key": "overlay_fps", "title": "帧率/帧时间曲线", "type": "boolean", "default": False},
      {"key": "overlay_cpu", "title": "CPU 占用", "type": "boolean", "default": False},
      {"key": "overlay_opacity", "title": "不透明度", "type": "number", "min": 0, "max": 100, "step": 5, "default": 80}]},
    {"id": "advanced", "title": "高级", "advanced": True, "items": [
      {"key": "deferred_readback", "title": "延迟回读", "type": "boolean", "default": True},
      {"key": "stale_readback", "title": "容忍过期回读", "type": "boolean", "default": False},
      {"key": "frames_queued", "title": "GPU 排队帧数", "type": "number", "min": 0, "max": 8, "step": 1, "default": 0,
       "note": "0 = 不限制"},
      {"key": "debug_view", "title": "调试视图", "type": "boolean", "default": False},
      {"key": "vibration", "title": "震动反馈", "type": "boolean", "default": True}]}
  ]}

def index(schema):
    out = {}
    for g in schema["groups"]:
        for it in g["items"]:
            out[it["key"]] = it
    return out

def read_ini(path):
    vals = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            vals[k.strip()] = v.strip()
    return vals

def write_ini(path, vals):
    shutil.copyfile(path, path + ".bak")
    known = index(SCHEMA)
    lines = []
    seen = set()
    for line in open(path, encoding="utf-8", errors="replace").read().splitlines():
        s = line.strip()
        key = s.split("=", 1)[0].strip() if "=" in s and not s.startswith(("#", ";")) else None
        if key is not None and key in known:          # schema 内键重写
            lines.append(f"{key}={vals[key]}")
            seen.add(key)
        else:                                          # 未知键原样保留（launcher 资产）
            lines.append(line)
    for k, v in vals.items():
        if k not in seen:
            lines.append(f"{k}={v}")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    import os
    os.replace(tmp, path)

def die(msg):
    print(json.dumps({"error": msg}))
    raise SystemExit(1)

def coerce(it, raw):
    t = it["type"]
    if t == "boolean":
        v = raw if isinstance(raw, bool) else str(raw).strip().lower() in ("1", "true", "yes", "on")
        return "1" if v else "0"
    if t == "number":
        v = float(raw)
        if "min" in it: v = max(it["min"], v)
        if "max" in it: v = min(it["max"], v)
        step = it.get("step")
        if step:
            digits = len(str(step).split(".")[-1]) if "." in str(step) else 0
            v = round(v, digits if digits < 3 else 3)
        return (f"{v:g}")
    if t == "enum":
        choices = [str(c) for c in it["enum"]]
        v = str(raw)
        if v not in choices:
            raise ValueError(f"{it['key']}: {v!r} not in enum")
        return v
    return str(raw)                                    # string

sub, ini = sys.argv[1], sys.argv[2]
known = index(SCHEMA)
if sub == "schema":
    print(json.dumps(SCHEMA))
elif sub == "get":
    ini_vals = read_ini(ini)
    out = {}
    for k, it in known.items():
        if k in ini_vals:
            try:
                out[k] = coerce(it, ini_vals[k])
            except Exception:
                out[k] = it.get("default")
        elif "default" in it:
            out[k] = it.get("default")
    print(json.dumps(out))
elif sub == "set":
    payload = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
    if not isinstance(payload, dict):
        die('payload must be an object')
    ini_vals = read_ini(ini)
    for k, v in payload.items():
        if k not in known:
            die(f'key not in schema: {k}')
        ini_vals[k] = coerce(known[k], v)
    write_ini(ini, ini_vals)
    print(json.dumps({"ok": True, "written": {k: ini_vals[k] for k in payload}}))
else:
    die('usage: config schema|get|set')
PYEOF
    ;;
info)
    sub="${1:-}"
    [ "$sub" = "patches" ] || { printf '{"error":"usage: info patches"}\n'; exit 2; }
    py_exec - "$OUT/patch_status.json" <<'PYEOF'
import json, sys, os
path = sys.argv[1]
if not os.path.isfile(path):
    print(json.dumps({"requested": [], "effective": {},
                      "note": "run the game once; patch status appears after launch"}))
    raise SystemExit(0)
d = json.load(open(path, encoding="utf-8"))
applied = d.get("applied") or []
req = [{"name": n, "status": "applied"} for n in applied]
if d.get("reason"):
    req.append({"name": "patch selection", "status": "info", "reason": d["reason"]})
eff = {"title_id": d.get("title_id"), "app_version": d.get("app_version"),
       "vulkan_status": d.get("vulkan_status"), "supported": d.get("supported")}
for k in ("dmem_mb", "vblank_hz", "upscaler"):
    if k in d: eff[k] = d[k]
print(json.dumps({"requested": req, "effective": eff}))
PYEOF
    ;;
*)
    printf '{"error":"usage: entry.sh game|launcher|check|vulkan|config schema|get|set|info patches"}\n'
    exit 2
    ;;
esac
