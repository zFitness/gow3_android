#!/usr/bin/env bash
# 推送构建好的 Port v2 包到 fexdroid debug app（真机调试循环）。
# 用法：bash packaging/arm-linux/deploy.sh [imports 目录名，默认 spartanport]
# 前置：build.sh 已跑完（out/arm64-linux/pkg + dist/SpartanPort-arm64.AppImage）；adb 连机。
# App 的 v2 导入流水线（解压/删原件）尚未交付（port-appimage-model tasks 4.x）——
# 调试期在这里本地解压（unsquashfs 按 runtime 偏移），直装「port.json + squashfs-root/」，
# 与导入流水线的产出形态一致；home/（storage.home）原样保留。
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
PKG=${FXD_PACKAGE:-ro.cobrabm.fexdroid}
NAME=${1:-spartanport}
PKGDIR=out/arm64-linux/pkg
APPIMAGE=$PKGDIR/SpartanPort-arm64.AppImage
ADB=(adb); [[ -n ${ANDROID_SERIAL:-} ]] && ADB+=(-s "$ANDROID_SERIAL")
[[ -f $PKGDIR/port.json ]] || { echo "no package: $PKGDIR (run packaging/arm-linux/build.sh)" >&2; exit 1; }
[[ -f $APPIMAGE ]] || { echo "no AppImage: $APPIMAGE" >&2; exit 1; }

# ---- 本地解压（--appimage-extract 的等价物）：squashfs 追加在 runtime ELF 之后 ----
runtime=$([[ -d ../fexdroid ]] && echo ../fexdroid/build/tools/runtime-aarch64 || echo "")
[[ -s $runtime ]] || { echo "runtime blob not found: $runtime" >&2; exit 1; }
offset=$(stat -f%z "$runtime")
extracted=out/arm64-linux/extracted
rm -rf "$extracted"
mkdir -p "$extracted/squashfs-root"
unsquashfs -no-progress -o "$offset" -d "$extracted/squashfs-root" "$APPIMAGE" >/dev/null
# port.json / assets 不在镜像里（标签与封面在包根），补齐
cp -f "$PKGDIR/port.json" "$extracted/"
[[ -d $PKGDIR/assets ]] && cp -R "$PKGDIR/assets" "$extracted/"

echo ">> run-as 直装：files/imports/${NAME}（home/ 保留）"
# macOS tar 会打 ._ AppleDouble 垃圾——排除；管道直装不落 /sdcard
COPYFILE_DISABLE=1 tar --exclude='._*' -C "$extracted" -cf - . | "${ADB[@]}" exec-in run-as "$PKG" sh -c \
  "mkdir -p 'files/imports/$NAME' && cd 'files/imports/$NAME' && rm -rf gport squashfs-root port.json assets && tar -xf -"

echo ">> 完成。v2 布局：port.json + squashfs-root/ + home/"
echo "   程序 = <rootfs>/bin/sh   参数 = imports/$NAME/squashfs-root/AppRun game      （一键进游戏）"
echo "   参数 = imports/$NAME/squashfs-root/AppRun launcher （PC 配置器）"
echo "   （首跑诊断：files/imports/$NAME/home/gport/logs/launch.log；Vulkan preflight 失败 exit 4）"
