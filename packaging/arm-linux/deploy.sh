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
# 游戏正在运行时替换程序树 → 运行中的实例可能读到半截/缺失的文件 → 一次性野指针崩溃。
# 提示但仍继续（调试期常在跑着游戏时热更）。
if "${ADB[@]}" shell "ps -A 2>/dev/null | grep -q spartanport-pro"; then
  echo "!! 警告：设备上游戏正在运行（spartanport-probe）——请留意是否出现读档/进游戏崩溃。" >&2
fi
# macOS tar 会打 ._ AppleDouble 垃圾——排除；管道直装不落 /sdcard
#
# 两阶段直装（对着 adb 连接会被打断的现实）：
#   阶段 1  把树打成 tar 用管道传成设备上的 .stage.tar（管道断了也只是文件残缺，可重传）
#   阶段 2  用独立 adb 命令解包 + 校验文件数 + 原子切换（不依赖管道存活）
# 原子切换的关键：旧树整体 rename 走 → 运行中的进程仍持有旧 inode，不会读到半截文件；
# 解包或校验失败 → 旧树原封不动，设备上不留半铺状态。
expected=$(find "$extracted" -type f | wc -l | tr -d ' ')
tarball=out/arm64-linux/extracted.tar
stagesh=out/arm64-linux/stage.sh
COPYFILE_DISABLE=1 tar --exclude='._*' -C "$extracted" -cf "$tarball" .
want_size=$(stat -f%z "$tarball")
cat > "$stagesh" <<EOS
set -e
cd "files/imports/$NAME"
rm -rf .stage.new .stage.old
mkdir -p .stage.new
tar -xf .stage.tar -C .stage.new
got=\$(find .stage.new -type f | wc -l | tr -d ' ')
if [ "\$got" != "$expected" ]; then
  echo "文件数不符 got=\$got want=$expected" >&2
  rm -rf .stage.new
  exit 1
fi
mkdir -p .stage.old
[ -d squashfs-root ] && mv squashfs-root .stage.old/ || true
[ -f port.json ] && mv port.json .stage.old/ || true
[ -d assets ] && mv assets .stage.old/ || true
mv .stage.new/squashfs-root .
[ -f .stage.new/port.json ] && mv .stage.new/port.json . || true
[ -d .stage.new/assets ] && mv .stage.new/assets . || true
rm -rf .stage.old .stage.new
echo switched
EOS
"${ADB[@]}" exec-in run-as "$PKG" sh -c "cat > files/imports/$NAME/.stage.tar" < "$tarball"
"${ADB[@]}" exec-in run-as "$PKG" sh -c "cat > files/imports/$NAME/.stage.sh" < "$stagesh"
sleep 1
got_size=$("${ADB[@]}" shell "run-as $PKG sh -c 'stat -c%s files/imports/$NAME/.stage.tar 2>/dev/null'" | tr -d '\r\n')
if [[ $got_size != "$want_size" ]]; then
  echo "!! 传输不完整（设备 $got_size / 本地 $want_size 字节）→ 保留旧树，未替换" >&2
  exit 1
fi
if ! "${ADB[@]}" shell "run-as $PKG sh files/imports/$NAME/.stage.sh"; then
  echo "!! 直装未通过校验：已保留设备上的旧程序树（未替换）" >&2
  "${ADB[@]}" shell "run-as $PKG sh -c 'rm -rf files/imports/$NAME/.stage.new'" >&2
  exit 1
fi
"${ADB[@]}" shell "run-as $PKG sh -c 'rm -f files/imports/$NAME/.stage.tar files/imports/$NAME/.stage.sh files/imports/$NAME/.stage.log'"
echo ">> 切换完成（原子替换，文件数校验通过：${expected}）"

echo ">> 完成。v2 布局：port.json + squashfs-root/ + home/"
echo "   程序 = <rootfs>/bin/sh   参数 = imports/$NAME/squashfs-root/AppRun game      （一键进游戏）"
echo "   参数 = imports/$NAME/squashfs-root/AppRun launcher （PC 配置器）"
echo "   （首跑诊断：files/imports/$NAME/home/gport/logs/launch.log；Vulkan preflight 失败 exit 4）"
