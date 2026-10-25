#!/usr/bin/env bash
# 推送构建好的 Runtime 包到 fexdroid debug app（真机调试循环）。
# 用法：bash packaging/arm-linux/deploy.sh [imports 目录名，默认 spartanport]
# 前置：build.sh 已跑完（out/arm64-linux/pkg/）；adb 连机；app 为 debuggable（run-as 可用）。
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
PKG=${FXD_PACKAGE:-ro.cobrabm.fexdroid}
NAME=${1:-spartanport}
STAGE=out/arm64-linux/pkg
ADB=(adb); [[ -n ${ANDROID_SERIAL:-} ]] && ADB+=(-s "$ANDROID_SERIAL")
[[ -f $STAGE/runtime.json ]] || { echo "no package: $STAGE (run packaging/arm-linux/build.sh)" >&2; exit 1; }

echo ">> run-as 直装：files/imports/$NAME"
tar -C "$STAGE" -cf - . | "${ADB[@]}" exec-in run-as "$PKG" sh -c \
  "mkdir -p 'files/imports/$NAME/gdata' 'files/imports/$NAME/home/$NAME' && cd 'files/imports/$NAME' && rm -rf gport runtime.json assets && tar -xf -"

echo ">> 完成。fexdroid Runner 自定义程序："
echo "   程序 = <rootfs>/bin/sh   参数 = imports/$NAME/gport/entry.sh game"
echo "   （首跑诊断：files/imports/$NAME/gdata/logs/；entry.sh 的 Vulkan preflight 失败 exit 4）"
