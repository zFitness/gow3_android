#!/usr/bin/env bash
# 推送构建好的 Runtime 包到 fexdroid debug app（真机调试循环）。
# 用法：bash packaging/arm-linux/deploy.sh [imports 目录名，默认 spartanport]
# 前置：build.sh 已跑完（out/arm64-linux/pkg/）；adb 连机；app 为 debuggable（run-as 可用）。
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
PKG=${FXD_PACKAGE:-ro.cobrabm.fexdroid}
NAME=${1:-spartanport}
STAGE=out/arm64-linux/pkg
STAGING=/sdcard/local/tmp/sp-runtime
FILES=/data/user/0/$PKG/files
ADB=(adb); [[ -n ${ANDROID_SERIAL:-} ]] && ADB+=(-s "$ANDROID_SERIAL")
[[ -f $STAGE/runtime.json ]] || { echo "no package: $STAGE (run packaging/arm-linux/build.sh)" >&2; exit 1; }

echo ">> push 中转：$STAGE → $STAGING"
"${ADB[@]}" shell mkdir -p "$STAGING"
tar -C "$STAGE" -cf - . | "${ADB[@]}" shell "cat > $STAGING/pkg.tar"
echo ">> run-as 拷入：files/imports/$NAME"
"${ADB[@]}" shell run-as "$PKG" sh -c "
    mkdir -p 'files/imports/$NAME/gdata' 'files/imports/$NAME/home/$NAME' &&
    rm -rf 'files/imports/$NAME/gport' &&
    (cd '$STAGING' && tar -xf pkg.tar) &&
    mv '$STAGING/gport' 'files/imports/$NAME/gport' &&
    mv '$STAGING/runtime.json' 'files/imports/$NAME/runtime.json' &&
    rm -rf '$STAGING'"
echo ">> 完成。fexdroid Runner 自定义程序："
echo "   程序 = <rootfs>/bin/sh   参数 = imports/$NAME/gport/entry.sh game"
echo "   （或者建一个 wrapper 脚本烤 SPARTAN_GAME_DIR；真机首跑看 files/imports/$NAME/gdata/logs/）"
