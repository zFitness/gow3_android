# 一键入口（fexdroid 尚未支持 runtime.json 适配期的过渡形态）。
# fexdroid Runner 自定义程序：程序 = <rootfs>/bin/sh，参数 = imports/spartanport/launch.sh
PKG=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# 调试期缺省：共享存储的游戏 dump（fexdroid 注入 manifest env 后此处仅为兜底）
export SPARTAN_GAME_DIR="${SPARTAN_GAME_DIR:-/storage/emulated/0/games/ps4/GOW3 ROM/CUSA01740}"
exec "$PKG/gport/entry.sh" game "$@"
