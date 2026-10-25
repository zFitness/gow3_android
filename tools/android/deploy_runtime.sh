#!/usr/bin/env bash
# 阶段一部署管道（fexdroid debug app 真机调试）：bundle 程序树 → adb push 中转 →
# run-as 拷入 fexdroid imports/，并生成阶段一手工入口 wrapper（阶段二由
# runtime.json + entry.sh 取代；见 my-docs/Runtime打包规范.md 与迁移 change design）。
#
# Usage: tools/android/deploy_runtime.sh [imports 目录名，默认 spartanport]
#   前置：adb 连机（app 需 debuggable / run-as 可用）；bundle 已由
#   tools/android/build/build_runtime.sh 产出（out/arm64/bundle/）。
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
PKG="${FXD_PACKAGE:-ro.cobrabm.fexdroid}"
NAME="${1:-spartanport}"
SRC="${GPORT_BUNDLE:-out/arm64/bundle}"
STAGING=/sdcard/local/tmp/sp-runtime
FILES=/data/user/0/$PKG/files
DEST="files/imports/$NAME"
ADB=(adb); [[ -n ${ANDROID_SERIAL:-} ]] && ADB+=(-s "$ANDROID_SERIAL")

[[ -d $SRC ]] || { echo "no bundle: $SRC (run tools/android/build/build_runtime.sh)" >&2; exit 1; }

echo ">> push 中转：$SRC → $STAGING（/sdcard FUSE 读取走应用存储授权）"
"${ADB[@]}" shell mkdir -p "$STAGING"
tar -C "$SRC" -cf - . | "${ADB[@]}" shell "cat > $STAGING/bundle.tar"
"${ADB[@]}" shell "cd $STAGING && tar -xf bundle.tar && rm bundle.tar"

echo ">> run-as 拷入：$DEST"
"${ADB[@]}" shell run-as "$PKG" sh -c "
    mkdir -p '$DEST/gdata' '$DEST/home' &&
    cp -r '$STAGING'/. '$DEST/gport/' &&
    rm -rf '$STAGING'"

echo ">> 生成阶段一手工入口：$DEST/launch.sh"
"${ADB[@]}" shell run-as "$PKG" sh -c "cat > '$DEST/launch.sh' && chmod 755 '$DEST/launch.sh'" <<'WRAPPER'
#!/system/bin/sh
# 阶段一手工入口：PC 原链（run.py → spartanport-probe）。
# fexdroid Runner 自定义程序：程序 = <rootfs>/bin/sh，参数 = imports/<name>/launch.sh game
HOST=/data/user/0/ro.cobrabm.fexdroid/files/rootfs
BASE=/data/user/0/ro.cobrabm.fexdroid/files/imports/spartanport
export SPARTAN_PORT_DIR="$BASE/gport"
export SPARTAN_DATA_DIR="$BASE/gdata"
export SPARTAN_HOME="$BASE/home/gport"
export SPARTAN_GAME_DIR="${SPARTAN_GAME_DIR:-/storage/emulated/0/Download/games/ps4/GOW3 ROM/CUSA01740}"
mkdir -p "$SPARTAN_DATA_DIR/user" "$SPARTAN_DATA_DIR/logs" "$SPARTAN_HOME"
# guest glibc：桌面同款 hwcaps 关闭（FEXCore 下不受支持指令）
export GLIBC_TUNABLES='glibc.cpu.hwcaps=-AVX2,-AVX,-AVX_Fast_Unaligned_Load,-AVX2_Usable,-AVX_Usable'
export SDL_VIDEODRIVER=x11 DISPLAY=:0
export MESA_VK_WSI_DEBUG=sw MESA_VK_WSI_PRESENT_MODE=relaxed
export FEXDROID_PRESENT="${FXD_ROOT:-$HOST}/tmp/fxpresent.sock"
export SDL_JOYSTICK_HIDAPI=0
[ -f "$SPARTAN_PORT_DIR/config/gport-gamecontrollerdb.txt" ] &&
    export SDL_GAMECONTROLLERCONFIG_FILE="$SPARTAN_PORT_DIR/config/gport-gamecontrollerdb.txt"
export PULSE_SERVER="unix:${FXD_ROOT:-$HOST}/tmp/pulse/native" PULSE_LATENCY_MSEC=60
# 手柄文件桥（fexdroid 无条件注入；兜底显式路径）
export SPARTAN_PAD_FILE="${SPARTAN_PAD_FILE:-/data/user/0/ro.cobrabm.fexdroid/files/android-pad.state}"
export LD_LIBRARY_PATH="$SPARTAN_PORT_DIR/arm64/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$SPARTAN_PORT_DIR"
exec "$HOST/usr/bin/python3.13" run.py game "$@"
WRAPPER

echo ">> 完成。fexdroid Runner 里建自定义程序："
echo "   程序 = <rootfs>/bin/sh   参数 = imports/$NAME/launch.sh game"
echo "   （真机首跑看 $DEST/gdata/logs/ 下的日志；Vulkan preflight 失败 exit 4）"
