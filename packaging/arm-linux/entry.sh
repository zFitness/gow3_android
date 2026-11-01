#!/bin/sh
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
# 用户数据统一 HOME（单一备份/导出单元，2026-10-26 起 gdata 布局退役）
GHOME="${SPARTAN_HOME:-$PKG/home/gport}"
DATA="$GHOME"
CONFIG="${SPARTAN_CONFIG:-$DATA/spartanport.ini}"
OUT="$DATA/out"
# 调试期缺省：共享存储的游戏 dump（fexdroid 注入 manifest env 后此处仅为兜底）
# 游戏目录优先级：env > ini 的 game_dir 配置项（config file 项，§7）> 烤入缺省
_ini_dir=""; [ -f "$CONFIG" ] && { _ini_dir=$(sed -n 's/^game_dir=//p' "$CONFIG" | tail -1) || true; } || true
GAME_DIR="${SPARTAN_GAME_DIR:-${_ini_dir:-/storage/emulated/0/Download/games/ps4/GOW3 ROM/CUSA01740}}"
HOST="${FXD_ROOT:-/data/user/0/ro.cobrabm.fexdroid/files/rootfs}"
PY=python3.13; command -v "$PY" >/dev/null 2>&1 || PY=python3
BIN="$TREE/bin"
LIB="$TREE/lib"
PROBE="$BIN/spartanport-probe"
# python 统一走显式 loader（烤入的解释器路径在跨 app/无会话场景不可靠；旧链同款模式）。
# run-as/无会话下 PATH 里没有 rootfs bin，必须用 rootfs 绝对路径 + 补 libpython 查找路径。
LOADER="$HOST/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
py_exec() {
    if [ -x "$LOADER" ] && [ -x "$HOST/usr/bin/python3.13" ]; then
        LD_LIBRARY_PATH="$HOST/usr/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            exec "$LOADER" "$HOST/usr/bin/python3.13" "$@"
    fi
    exec "$PY" "$@"
}

launch_env() {
    mkdir -p "$DATA/user" "$DATA/logs" "$OUT" "$GHOME"
    # config 项（config_cli 同一契约）：video_backend / pad_shim / readback_linear
    _cfg() { [ -f "$CONFIG" ] && sed -n "s/^$1=//p" "$CONFIG" | tail -1; }
    _cfg_bool() { v=$(_cfg "$1") || true; [ "$v" != "0" ] && [ "$v" != "false" ]; }
    _cfg_backend=$(_cfg video_backend) || _cfg_backend="";
    # 旧 gdata 布局一次性并入 HOME（存档/配置优先，不覆盖新数据）
    if [ -d "$PKG/gdata" ] && [ ! -e "$DATA/.gdata-migrated" ]; then
        cp -R "$PKG/gdata/user/." "$DATA/user/" 2>/dev/null || true
        cp -f "$PKG/gdata/spartanport.ini" "$DATA/" 2>/dev/null || true
        cp -R "$PKG/gdata/logs/." "$DATA/logs/" 2>/dev/null || true
        : > "$DATA/.gdata-migrated"
    fi
    [ -f "$CONFIG" ] || : > "$CONFIG"
    export SPARTAN_PORT_DIR="$TREE" SPARTAN_DATA_DIR="$DATA" SPARTAN_HOME="$GHOME"
    # 上游的慢等待诊断（src/runtime_thread.c）：guest 的等待超过这个毫秒数就打印是哪一把锁/条件
    # 变量卡住了——FEX 路径上黑屏时进程还活着但不出帧，靠它能看出卡在哪。
    export SPARTAN_SLOW_WAITS="${SPARTAN_SLOW_WAITS:-2000}"
    export SPARTAN_CONFIG="$CONFIG" SPARTAN_USER_DIR="$DATA/user" SPARTAN_LOG_DIR="$DATA/logs"
    export SPARTAN_GAME_DIR="$GAME_DIR"
    # guest glibc：桌面同款 hwcaps 关闭（FEXCore 下不受支持指令）
    export GLIBC_TUNABLES='glibc.cpu.hwcaps=-AVX2,-AVX,-AVX_Fast_Unaligned_Load,-AVX2_Usable,-AVX_Usable'
    export SDL_VIDEODRIVER=x11 DISPLAY="${DISPLAY:-:0}"
    case "${_cfg_backend:-auto}" in
        x11|wayland) export SDL_VIDEODRIVER="$_cfg_backend" ;;
    esac
    export MESA_VK_WSI_DEBUG=sw MESA_VK_WSI_PRESENT_MODE=relaxed
    export FEXDROID_PRESENT="${FEXDROID_PRESENT:-$HOST/tmp/fxpresent.sock}"
    export SDL_JOYSTICK_HIDAPI=0
    if [ -f "$TREE/config/gport-gamecontrollerdb.txt" ]; then
        export SDL_GAMECONTROLLERCONFIG_FILE="$TREE/config/gport-gamecontrollerdb.txt"
    fi
    export PULSE_SERVER="${PULSE_SERVER:-unix:$HOST/tmp/pulse/native}" PULSE_LATENCY_MSEC=60
    # 打包库（rootfs 缺的 SDL3/ffmpeg 等）；bin/gpu 由 probe 的 RUNPATH 自取。
    # rootfs 的 lib 也挂上：供 fexpad 预载库解析 libc 等
    export LD_LIBRARY_PATH="$LIB:$HOST/usr/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export PATH="$BIN:$PATH"
    export HOME="${HOME:-$GHOME}"   # Tk/配置文件落在 guest home
    # Tcl/Tk 数据文件：_tkinter 用编译期默认路径找 init.tcl（guest 视角未映射），显式指到 rootfs
    export TCL_LIBRARY="$HOST/usr/share/tcltk/tcl8.6"
    export TK_LIBRARY="$HOST/usr/share/tcltk/tk8.6"
    # fexpad 虚拟手柄：只注入 probe（游戏的 SDL3 在 probe 进程），经 SPARTAN_PAD_LIB 由
    # probe 自加载——全局 LD_PRELOAD 会让链路上的 python/sed/tr 各干等 30 秒 SDL3 重试
    # （迁移专栏 09 轮次 8），launcher/链路慢到不可用。config 的 pad_shim=0 关闭。
    if _cfg_bool pad_shim && [ -f "$HOST/usr/lib/fexdroid/libfexpad.so" ]; then
        export SPARTAN_PAD_LIB="$HOST/usr/lib/fexdroid/libfexpad.so"
    fi
    # config 的 readback_linear（launch_env.py 消费，缺省开）
    if ! _cfg_bool readback_linear; then
        export SPARTAN_READBACK_LINEAR=0
    fi
    # config 的补丁选择（patches_enabled + "patch: 名字" 键）→ run.py 的 SPARTAN_PATCHES/ONLY。
    # 缺省关（与 schema 的 default:false 一致——不能用 _cfg_bool，它把空值当开）。
    # 开了但没勾任何补丁 = 维持缺省，不导出。if 体收尾不挂 && 链（set -e 会吞掉整个脚本）。
    if [ "$(_cfg patches_enabled)" = "1" ] && [ -f "$CONFIG" ]; then
        _sel=$(sed -n 's/^patch: \(.*\)=1$/\1/p' "$CONFIG" | tr '\n' ';')
        _sel=${_sel%;}
        if [ -n "$_sel" ]; then
            export SPARTAN_PATCHES="$_sel" SPARTAN_PATCHES_ONLY=1
        fi
    fi
}

verb="${1:-}"; [ $# -gt 0 ] && shift || true

case "$verb" in
game|game-direct|run)
    launch_env
    # 链路自记录：准备/启动的一切诊断落盘（闪退时无需 app 会话内抓取）
    exec >> "$DATA/logs/launch.log" 2>&1
    echo "=== launch $(date) uid=$(id -u) ==="
    [ -n "$GAME_DIR" ] || { echo "Runtime: SPARTAN_GAME_DIR is not set" >&2; exit 2; }
    export SPARTAN_PROBE="$PROBE"
    cd "$TREE"
    py_exec run.py "$@"
    ;;
launcher)
    launch_env
    { echo "=== launcher $(date) uid=$(id -u) DISPLAY=$DISPLAY"; [ -x "$LOADER" ] && echo "LOADER ok" || echo "LOADER MISSING"; } >> "$DATA/logs/launch.log" 2>&1 || true
    if ! "$PY" -c 'import tkinter' 2>/dev/null; then
        printf '{"error":"tkinter missing: install python3-tk into the rootfs (fexdroid scripts/add-python313-tk.sh)"}\n'
        exit 3
    fi
    echo "launcher: tkinter ok, starting UI" >> "$DATA/logs/launch.log" 2>&1 || true
    # gport: Rust 启动器开关——ini 里 `launcher=rust`（或 SPARTAN_RUST_LAUNCHER=rust/1）
    # 且 bin/ 下有 rust 版时走它；缺省仍是 Python 启动器。切换后重开会话生效。
    _cfg_launcher=$(_cfg launcher) || _cfg_launcher=""
    if [ "${SPARTAN_RUST_LAUNCHER:-$_cfg_launcher}" = "rust" ] && [ -x "$BIN/spartanport-launcher-rust" ]; then
        echo "launcher: starting the RUST launcher" >> "$DATA/logs/launch.log" 2>&1 || true
        exec "$LOADER" --library-path "$TREE/lib:$HOST/usr/lib/aarch64-linux-gnu:$HOST/usr/lib" \
            "$BIN/spartanport-launcher-rust" 2>> "$DATA/logs/launch.log"
    fi
    py_exec "$TREE/launcher/spartanport_launcher_win.py" 2>> "$DATA/logs/launch.log"
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
    [ -d "$DATA/logs" ] || mkdir -p "$DATA/logs"
    [ -d "$DATA" ] || mkdir -p "$DATA"
    [ -f "$CONFIG" ] || : > "$CONFIG"
    # 排障痕迹：App 侧 PortVerbs 丢弃 stderr，127 类失败只有这里能取证（stdout 照常给 App）
    { echo "=== config $(date) sub=$sub uid=$(id -u)"; echo "HOST=$HOST"; echo "PATH=$PATH"; [ -x "$LOADER" ] && echo "LOADER exec: yes" || echo "LOADER exec: NO"; [ -x "$HOST/usr/bin/python3.13" ] && echo "rootfs py: yes" || echo "rootfs py: NO"; command -v python3.13 >/dev/null 2>&1 && echo "PATH py3.13: yes" || echo "PATH py3.13: NO"; } >> "$DATA/logs/config.log" 2>&1 || true
    py_exec "$TREE/entry_main.py" config "$sub" "$CONFIG" "$@" 2>> "$DATA/logs/config.log" || { code=$?; echo "config: py_exec exit $code" >> "$DATA/logs/config.log"; exit $code; }
    ;;
info)
    sub="${1:-}"; [ $# -gt 0 ] && shift || true
    py_exec "$TREE/entry_main.py" info "$sub" "$OUT/patch_status.json"
    ;;
*)
    printf '{"error":"usage: entry.sh game|launcher|check|vulkan|config schema|get|set|info patches"}\n'
    exit 2
    ;;
esac
