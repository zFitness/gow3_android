#!/usr/bin/env bash
# 推送构建好的 Port 包到 fexdroid debug app（真机调试循环）。
# 用法：bash packaging/arm-linux/deploy.sh [imports 目录名，默认 spartanport]
# 前置：build.sh 已跑完（out/arm64-linux/pkg/）；adb 连机；app 为 debuggable（run-as 可用）。
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
PKG=${FXD_PACKAGE:-ro.cobrabm.fexdroid}
NAME=${1:-spartanport}
STAGE=out/arm64-linux/pkg
ADB=(adb); [[ -n ${ANDROID_SERIAL:-} ]] && ADB+=(-s "$ANDROID_SERIAL")
[[ -f $STAGE/port.json ]] || { echo "no package: $STAGE (run packaging/arm-linux/build.sh)" >&2; exit 1; }

echo ">> run-as 直装：files/imports/$NAME"
# macOS tar 会打 ._ AppleDouble 垃圾——排除；管道直装不落 /sdcard
COPYFILE_DISABLE=1 tar --exclude='._*' -C "$STAGE" -cf - . | "${ADB[@]}" exec-in run-as "$PKG" sh -c \
  "mkdir -p 'files/imports/$NAME/home/$NAME' && cd 'files/imports/$NAME' && rm -rf gport port.json assets && tar -xf -"

echo ">> 完成。入口来自 port.json（App 解析），调试可用："
echo "   程序 = <rootfs>/bin/sh   参数 = imports/$NAME/gport/entry.sh game      （一键进游戏）"
echo "   参数 = imports/$NAME/gport/entry.sh launcher （PC 配置器）"
echo "   （首跑诊断：files/imports/$NAME/home/gport/logs/launch.log；Vulkan preflight 失败 exit 4）"
