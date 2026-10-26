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
# 调试期缺省：共享存储的游戏 dump（fexdroid 注入 manifest env 后此处仅为兜底）
GAME_DIR="${SPARTAN_GAME_DIR:-/storage/emulated/0/games/ps4/GOW3 ROM/CUSA01740}"
HOST="${FXD_ROOT:-/data/user/0/ro.cobrabm.fexdroid/files/rootfs}"
PY=python3.13; command -v "$PY" >/dev/null 2>&1 || PY=python3
PROBE="$TREE/bin/spartanport-probe"
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
    [ -f "$CONFIG" ] || : > "$CONFIG"
    export SPARTAN_PORT_DIR="$TREE" SPARTAN_DATA_DIR="$DATA" SPARTAN_HOME="$GHOME"
    export SPARTAN_CONFIG="$CONFIG" SPARTAN_USER_DIR="$DATA/user" SPARTAN_LOG_DIR="$DATA/logs"
    # guest glibc：桌面同款 hwcaps 关闭（FEXCore 下不受支持指令）
    export GLIBC_TUNABLES='glibc.cpu.hwcaps=-AVX2,-AVX,-AVX_Fast_Unaligned_Load,-AVX2_Usable,-AVX_Usable'
    export SDL_VIDEODRIVER=x11 DISPLAY=:0
    export MESA_VK_WSI_DEBUG=sw MESA_VK_WSI_PRESENT_MODE=relaxed
    export FEXDROID_PRESENT="${FEXDROID_PRESENT:-$HOST/tmp/fxpresent.sock}"
    export SDL_JOYSTICK_HIDAPI=0
    [ -f "$TREE/config/gport-gamecontrollerdb.txt" ] &&
        export SDL_GAMECONTROLLERCONFIG_FILE="$TREE/config/gport-gamecontrollerdb.txt"
    export PULSE_SERVER="${PULSE_SERVER:-unix:$HOST/tmp/pulse/native}" PULSE_LATENCY_MSEC=60
    # 打包库（rootfs 缺的 SDL3/ffmpeg 等）；bin/gpu 由 probe 的 RUNPATH 自取。
    # rootfs 的 lib 也挂上：供 fexpad 预载库解析 libc 等
    export LD_LIBRARY_PATH="$TREE/lib:$HOST/usr/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export PATH="$TREE/bin:$PATH"
    export HOME="${HOME:-$GHOME}"   # Tk/配置文件落在 guest home
    # fexpad 虚拟手柄：顶层启动 shell 可能是 bionic（/system/bin/sh，fexdroid 对其不注入
    # preload），glibc 子树由这里自己补上——probe 进程内 fexpad 等到 SDL3 后注入虚拟手柄
    if [ -f "$HOST/usr/lib/fexdroid/libfexpad.so" ]; then
        export LD_PRELOAD="$HOST/usr/lib/fexdroid/libfexpad.so"
    fi
}

verb="${1:-}"; [ $# -gt 0 ] && shift || true

case "$verb" in
game|run)
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
    py_exec "$TREE/entry_main.py" config "$sub" "$CONFIG" "$@"
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
