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
# 阶段一树里还没有 entry.sh（pack.py 阶段才有）——直接从 runtime/ 源码侧补上
if [ -f runtime/entry.sh ]; then
  "${ADB[@]}" shell run-as "$PKG" sh -c "cat > '$DEST/gport/entry.sh' && chmod 755 '$DEST/gport/entry.sh'" < runtime/entry.sh
fi

echo ">> 生成阶段一手工入口：$DEST/launch.sh"
"${ADB[@]}" shell run-as "$PKG" sh -c "cat > '$DEST/launch.sh' && chmod 755 '$DEST/launch.sh'" <<'WRAPPER'
#!/system/bin/sh
# 阶段一手工入口：转调 entry.sh（verb 协议；fexdroid Runner 自定义程序：
# 程序 = <rootfs>/bin/sh，参数 = imports/<name>/launch.sh game）
BASE=/data/user/0/ro.cobrabm.fexdroid/files/imports/spartanport
export SPARTAN_GAME_DIR="${SPARTAN_GAME_DIR:-/storage/emulated/0/Download/games/ps4/GOW3 ROM/CUSA01740}"
exec "$BASE/gport/entry.sh" "$@"
WRAPPER

echo ">> 完成。fexdroid Runner 里建自定义程序："
echo "   程序 = <rootfs>/bin/sh   参数 = imports/$NAME/launch.sh game"
echo "   （真机首跑看 $DEST/gdata/logs/ 下的日志；Vulkan preflight 失败 exit 4）"
