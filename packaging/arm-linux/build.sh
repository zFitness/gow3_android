#!/usr/bin/env bash
# packaging/arm-linux — 交叉构建 ARM64 Linux 版 SpartanPort 并打包成 fexdroid Runtime 包。
#
# 与根 build.sh Linux 分支的关系：同一构建步骤的 aarch64 交叉变体，差异只有三点——
#   1. 工具链：clang --target=aarch64-linux-gnu + Debian arm64 sysroot（fetch-sysroot.py 产出）
#   2. 跳过 x86 宿主专属源（isa_emulate/cpu_check/isa_rewrite：guest x86 代码由 FEXCore 执行）
#   3. probe 链接时烤入 rootfs 的 ELF 解释器（Android 沙箱没有 /lib）
# 运行入口 = run.py（与上游 packaged 形态一致：bin/spartanport-probe、bin/gpu/、scripts/），
# 程序树外另加 runtime.json + entry.sh（fexdroid 契约，见 my-docs/Runtime打包规范.md）。
#
# 用法：bash packaging/arm-linux/build.sh
#   SPARTAN_ARM64_SYSROOT  sysroot 路径（缺省自动 fetch；已有 gow_android_glibc 的则复用）
#   SPARTAN_ROOTFS_PREFIX  烤入 probe 的 ELF 解释器前缀（缺省 fexdroid rootfs）
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
out=$root/out/arm64-linux
program=${SPARTAN_PROGRAM:-gport}
dist=$root/packaging/arm-linux/dist

die() { echo "arm-linux: $*" >&2; exit 1; }

# ---- sysroot / 宿主头文件 -------------------------------------------------------------
sysroot=${SPARTAN_ARM64_SYSROOT:-$root/.local-deps/arm64-sysroot}
if [[ ! -d $sysroot/usr/lib/aarch64-linux-gnu ]]; then
    for cand in $root/.local-deps/android/arm64-sysroot \
                $root/../gow_android_glibc/.local-deps/android/arm64-sysroot; do
        [[ -d $cand/usr/lib/aarch64-linux-gnu ]] && { sysroot=$cand; break; }
    done
fi
if [[ ! -d $sysroot/usr/lib/aarch64-linux-gnu ]]; then
    echo ">> sysroot not found, fetching (packaging/arm-linux/fetch-sysroot.py)"
    # gport: Debian 自带的 Vulkan 头偏旧（缺 vk::DriverId::eMesaKosmickrisp 等），
    # 拉完后用本地的 Vulkan-Headers（ci-deps.sh 负责克隆）overlay 新头。
    vh=$root/.local-deps/Vulkan-Headers
    vh_args=""; [[ -d $vh/include/vulkan ]] && vh_args="--vulkan-headers $vh"
    python3 packaging/arm-linux/fetch-sysroot.py --out "$sysroot" $vh_args
fi
headers=${SPARTAN_ARM64_HEADERS:-}
if [[ -z $headers && -d $root/../gow_android_glibc/.local-deps/root/usr ]]; then
    headers=$root/../gow_android_glibc/.local-deps/root/usr
fi
headers=${headers:-$root/.local-deps/root/usr}
# tsl-robin-map 与 VulkanMemoryAllocator 是纯头文件库：本地放在 $headers 下，CI 用系统的
# （apt 的 robin-map-dev 提供的 cmake 包，或 /usr/local 装的 VMA）。
find_cmake_dir() {  # $1=包名
    local dir
    for dir in "$headers/share/cmake/$1" /usr/local/lib/cmake/"$1" /usr/lib/*/cmake/"$1" /usr/lib/cmake/"$1"; do
        [[ -d $dir ]] && { printf '%s' "$dir"; return; }
    done
}
tsl_dir=$(find_cmake_dir tsl-robin-map)
vma_dir=$(find_cmake_dir VulkanMemoryAllocator)
[[ -n $tsl_dir ]] || die "tsl-robin-map cmake config not found (headers: $headers)"
[[ -x $headers/bin/glslangValidator ]] && export PATH="$headers/bin:$PATH"

# ---- 工具链定位（macOS: brew llvm/lld/boost；Linux: 系统 llvm）-------------------------
llvm_bin=${SPARTAN_LLVM_BIN:-}
if [[ -z $llvm_bin ]]; then
    for cand in "$(brew --prefix llvm 2>/dev/null || true)/bin" /usr/lib/llvm-*/bin; do
        [[ -x $cand/clang && -x $cand/llvm-readelf ]] && { llvm_bin=$cand; break; }
    done
fi
[[ -n ${llvm_bin:-} && -x $llvm_bin/clang ]] || llvm_bin=$(command -v clang >/dev/null && { d=$(dirname "$(readlink -f "$(command -v clang)")"); [[ -x $d/llvm-readelf ]] && echo "$d"; } || true)
[[ -n ${llvm_bin:-} ]] || die 'need clang + llvm-readelf (brew install llvm, or set SPARTAN_LLVM_BIN)'
export PATH="$llvm_bin:$PATH"
command -v ld.lld >/dev/null 2>&1 || {
    lld_prefix=$(brew --prefix lld 2>/dev/null || true)
    [[ -n $lld_prefix && -d $lld_prefix/bin ]] && export PATH="$lld_prefix/bin:$PATH"
}
command -v ld.lld >/dev/null 2>&1 || die 'need ld.lld (brew install lld)'

boost_dir=${SPARTAN_BOOST_DIR:-}
if [[ -z $boost_dir ]]; then
    bp=$(brew --prefix boost 2>/dev/null || true)
    if [[ -n $bp && -d $bp/lib/cmake ]]; then
        boost_dir=$(find "$bp/lib/cmake" -maxdepth 1 -type d -name 'Boost-*' -print | sort | tail -n 1)
    fi
fi
if [[ -z $boost_dir ]]; then
    # Linux（CI）：系统 Boost 的 cmake 目录（Debian/Ubuntu 装在 /usr/lib/<triplet>/cmake/Boost-*）。
    boost_dir=$(find /usr/lib /usr/local/lib -maxdepth 3 -type d -name 'Boost-*' -path '*/cmake/*' 2>/dev/null | sort | tail -n 1)
fi
# conda 环境会漏 x86-64 专属 flags 进交叉编译（见 build_arm64.sh 的同款处理）。
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH CMAKE_PREFIX_PATH

# 交叉编译参数（probe/静态库直接用 clang；gpu/FEX 走 toolchain 文件）
cc=(clang --target=aarch64-linux-gnu --sysroot="$sysroot" -fuse-ld=lld)
tc=$root/packaging/arm-linux/cross-toolchain.cmake
export SPARTAN_ARM64_SYSROOT=$sysroot
export SPARTAN_ARM64_HEADER_PREFIX=$headers
export SPARTAN_ROOTFS_PREFIX=${SPARTAN_ROOTFS_PREFIX:-/data/user/0/ro.cobrabm.fexdroid/files/rootfs}

step=0; _t0=0
begin() { step=$((step + 1)); _t0=$(date +%s); printf '\n==> [%d/5] %s\n' "$step" "$*"; }
end_stage() { printf '    %s (%dm%02ds)\n' "$*" $(( ($(date +%s) - _t0) / 60 )) $(( ($(date +%s) - _t0) % 60 )); }

# 目标平台开关（与 gpu/CMakeLists.txt 的 option 同名）：SPARTAN_ARM = 目标是 ARM（ARM Linux 与
# 安卓共用这一份二进制，GPU 代码按它分支）；SPARTAN_NIXPORT = 目标是 nixport 平台（宿主服务
# 总线、移动端样式）。默认关，只有本构建线显式打开。
# 同一组值也写进程序树的 config/gport-build-flags.json（Python 侧 scripts/build_flags.py 读它），
# C++ 与 Python 两侧的开关同源，不会漂移。
spartan_arm=ON
spartan_nixport=ON

# ---- 1/5 FEXCore + libgportcpu.so（guest CPU）-----------------------------------------
begin "FEXCore + libgportcpu.so"
if [[ ! -f $out/fex/build.ninja ]]; then
    cmake -S third_party/FEX -B "$out/fex" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$tc" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_FEX_INCLUDE="$root/cpu/fex_project.cmake" \
        -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_STEAM_SUPPORT=OFF \
        -DBUILD_FEX_LINUX_TESTS=OFF -DENABLE_ZYDIS=OFF -DTUNE_ARCH=armv8.2-a -DTUNE_CPU=generic >/dev/null
fi
ninja -C "$out/fex" gportcpu
end_stage "libgportcpu.so"

# ---- 2/5 libgportgpu.so（shadPS4 video core）------------------------------------------
begin "libgportgpu.so"
# 本端口对 FSR-Vulkan 的改动（gpu/patches/fsr-vulkan/*.patch，上游 3.0.0 的 FSR 放大与帧生成
# 依赖它）：打一次即可，重复跑时先试反向检查（上游 build.sh 同一写法）。
if [[ -d gpu/third_party/fsr-vulkan ]]; then
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        [[ -f $patch ]] || continue
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
        fi
    done
fi
# 目标平台开关变了要重新 configure：CMake 的 option 缓存在 build.ninja 里，不重跑不会生效。
if [[ ! -f $out/gpu/build.ninja ]] || ! grep -q "^SPARTAN_ARM:BOOL=$spartan_arm$" "$out/gpu/CMakeCache.txt" 2>/dev/null; then
    cmake -S gpu -B "$out/gpu" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$tc" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSPARTAN_PGO=off -DSPARTAN_LTO=OFF \
        -DSPARTAN_ARM="$spartan_arm" -DSPARTAN_NIXPORT="$spartan_nixport" \
        -DSPARTAN_ARM64_HEADER_PREFIX="$headers" \
        -Dtsl-robin-map_DIR="$tsl_dir" \
        ${vma_dir:+-DVulkanMemoryAllocator_DIR="$vma_dir"} \
        -DBoost_DIR="${boost_dir:-$headers/lib/cmake/Boost-1.87.0}" \
        -DZydis_DIR="$sysroot/usr/lib/aarch64-linux-gnu/cmake/zydis" >/dev/null
fi
ninja -C "$out/gpu" spartanportgpu
end_stage "libgportgpu.so"

# ---- 3/5 spartanport-probe（加载器；跳过 x86 宿主专属 ISA 源）---------------------------
begin "spartanport-probe"
export PKG_CONFIG_SYSROOT_DIR=$sysroot
export PKG_CONFIG_LIBDIR=$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/share/pkgconfig
export PKG_CONFIG_DONT_DEFINE_PREFIX=1
export PKG_CONFIG_FDO_SYSROOT_RULES=1
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3 libavcodec libavutil libswscale)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3 libavcodec libavutil libswscale)"
mkdir -p "$out/atrac9"
for source in third_party/LibAtrac9/C/src/*.c; do
    "${cc[@]}" -std=c99 -O2 -w -c "$source" -o "$out/atrac9/$(basename "${source%.c}").o"
done
"${llvm_bin}/llvm-ar" rcs "$out/libatrac9.a" "$out"/atrac9/*.o
mkdir -p "$out/bin/gpu"
"${cc[@]}" -std=c11 -O2 -g -Wall -Wextra -pthread -no-pie "${includes[@]}" -I. -Isrc \
    src/probe.c src/runtime*.c src/vulkan_smoke.c "$out/libatrac9.a" -lm \
    -L"$out/gpu" -lgportgpu -L"$out/fex" -lgportcpu -Wl,-rpath,'$ORIGIN/gpu' \
    -Wl,--dynamic-linker="$SPARTAN_ROOTFS_PREFIX/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" -rdynamic \
    "${libraries[@]}" -o "$out/bin/spartanport-probe"
install -m755 "$out/gpu/libgportgpu.so" "$out/bin/gpu/"
install -m755 "$out/fex/libgportcpu.so" "$out/bin/gpu/"
# ---- 3.5/5 Rust 启动器（可选）：有 cargo 就带上，没有则包回退 Python 启动器 ----------
begin "rust launcher (optional)"
if command -v cargo >/dev/null 2>&1; then
    bash launcher-rust/build-aarch64.sh
    install -m755 launcher-rust/target/aarch64-unknown-linux-gnu/release/spartanport-launcher \
        "$out/bin/spartanport-launcher-rust"
    echo "    Rust 启动器已构建"
else
    echo "    cargo 不可用：跳过（包回退 Python 启动器）"
fi
end_stage "rust launcher"
end_stage "probe + gpu/cpu libs"

# ---- 4/5 依赖库打包（sysroot 解析 NEEDED；rootfs 标配不打包）----------------------------
# 排除清单与 packaging/linux/build.sh 同源：这些由运行宿主（fexdroid rootfs 标配）提供，
# 打包它们反而会遮蔽驱动/显示/音频栈。
begin "bundle libraries"
excluded='^(ld-linux.*|libc|libm|libdl|librt|libpthread|libresolv|libutil|libnsl|libanl|libvulkan|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libgbm|libdrm.*|libX11|libX11-xcb|libxcb.*|libXau|libXdmcp|libXext|libXfixes|libXrender|libXi|libXcursor|libXrandr|libwayland-.*|libasound|libudev|libpulse.*|libfontconfig|libfreetype|libOpenCL|libva.*|libvdpau|libvpl|libnvidia-.*|libcuda|libz|libXss|libxkbcommon.*)\.so'
seen="|"  # bash 3.2（macOS 系统版）无关联数组，用字符串去重
mkdir -p "$out/lib"
resolve() {  # $1=库名 → sysroot 路径
    local hit
    hit=$(find "$sysroot/usr/lib/aarch64-linux-gnu" -maxdepth 1 -name "$1" -print -quit 2>/dev/null || true)
    printf '%s' "$hit"
}
bundle() {
    local elf=$1 line lib name target
    while read -r line; do
        lib=${line#*=> }
        lib=${lib% *}
        [[ $line == *'.so'* ]] || continue
        name=$(basename -- "${line#*=> }"); name=${name% *}
        case "$seen" in *"|$name|"*) continue ;; esac
        seen="$seen$name|"
        [[ $name =~ $excluded ]] && continue
        target=$(resolve "$name")
        [[ -n $target ]] || { echo "  not in sysroot, assuming rootfs: $name" >&2; continue; }
        install -m755 "$target" "$out/lib/$name"
        bundle "$target"
    done < <("$llvm_bin/llvm-readelf" -d "$elf" | awk '/\(NEEDED\)/{print $NF}' | tr -d '[]')
}
bundle "$out/bin/gpu/libgportgpu.so"
bundle "$out/bin/gpu/libgportcpu.so"
bundle "$out/bin/spartanport-probe"
if [[ -f $out/bin/spartanport-launcher-rust ]]; then
    bundle "$out/bin/spartanport-launcher-rust"
    # gport: 启动器运行时 dlopen 的库（winit X11 后端），NEEDED 扫描看不到 ⇒ 显式带上。
    # 依赖树：libxkbcommon-x11→{libxkbcommon,libX11-xcb,libxcb}、libxcb-xkb→libxcb，
    # 其余全是 rootfs 标配（排除清单已含）。
    for name in libxkbcommon-x11.so.0 libxcb-xkb.so.1; do
        target=$(find "$sysroot/usr/lib/aarch64-linux-gnu" -maxdepth 1 -name "$name" -print -quit 2>/dev/null || true)
        [[ -n $target ]] && install -m755 "$target" "$out/lib/$name"
    done
fi
find "$out/lib" -name '*.so*' -exec "$llvm_bin/llvm-strip" --strip-unneeded {} + 2>/dev/null || true
end_stage "$(ls "$out/lib" | wc -l | tr -d ' ') libs"

# ---- 5/5 AppDir 组装 + Port v2 AppImage（规范：Port打包规范.md，AppImage 分发模型） ------
begin "package (nixport port v2 appimage)"
appdir=$out/pkg/$program
rm -rf "$out/pkg"
mkdir -p "$appdir/bin/gpu" "$appdir/lib" "$appdir/config"
install -m755 "$out/bin/spartanport-probe" "$appdir/bin/"
if [[ -f $out/bin/spartanport-launcher-rust ]]; then
    install -m755 "$out/bin/spartanport-launcher-rust" "$appdir/bin/"
fi
install -m755 "$out/bin/gpu/libgportgpu.so" "$out/bin/gpu/libgportcpu.so" "$appdir/bin/gpu/"
cp -R "$out/lib/." "$appdir/lib/"
install -m755 packaging/arm-linux/entry.sh "$appdir/entry.sh"
install -m755 packaging/arm-linux/AppRun "$appdir/AppRun"
install -m644 packaging/arm-linux/entry_main.py "$appdir/entry_main.py"
install -m644 run.py "$appdir/"
cp -R scripts "$appdir/scripts"
find "$appdir/scripts" -name __pycache__ -prune -exec rm -r {} + 2>/dev/null || true
cp -R patches "$appdir/patches"
cp -R launcher "$appdir/launcher"
find "$appdir/launcher" -name __pycache__ -prune -exec rm -r {} + 2>/dev/null || true
for name in gport-fex-compatible.json gport-gamecontrollerdb.txt gport-packages.txt; do
    [[ -f packaging/arm-linux/config/$name ]] && install -m644 "packaging/arm-linux/config/$name" "$appdir/config/"
done
# 构建期开关的 Python 侧标记：与上面传给 CMake 的同一组值（scripts/build_flags.py 读它）。
# 桌面构建不写这个文件，两个开关都是关的。
python3 - "$appdir/config/gport-build-flags.json" "$spartan_arm" "$spartan_nixport" <<'PY'
import json
import sys

path, arm, nixport = sys.argv[1:4]
with open(path, 'w', encoding='utf-8') as out:
    json.dump({'arm': arm == 'ON', 'nixport': nixport == 'ON'}, out, indent=2)
    out.write('\n')
PY
# AppImage 三件套：.DirIcon（取启动器图标）+ .desktop
cp -f "$appdir/launcher/spartanport.png" "$appdir/.DirIcon"
install -m644 packaging/arm-linux/spartanport.desktop "$appdir/spartanport.desktop"
install -m644 packaging/arm-linux/port.json "$out/pkg/port.json"
# 封面不再由包携带：宿主从 AppImage 的 .DirIcon 派生（Port打包规范.md §6.3）。

# 验收：manifest / 布局 / shebang / 黑名单（驱动与游戏数据不得入包）
[[ $(python3 -c "import json;print(json.load(open('$out/pkg/port.json'))['schema'])") == nixport.port.v1 ]] || die 'port.json schema wrong'
[[ $(python3 -c "import json;print(json.load(open('$out/pkg/port.json'))['appimage'])") == SpartanPort-arm64.AppImage ]] || die 'port.json appimage field wrong'
# 新版 manifest 不再声明 arch / program / assets（目标架构由宿主从 ELF 头推导）。
for field in arch program assets; do
    python3 -c "import json,sys;sys.exit(0 if '$field' not in json.load(open('$out/pkg/port.json')) else 1)" \
        || die "port.json must not declare $field"
done
for f in "$appdir/entry.sh" "$appdir/AppRun" "$appdir/.DirIcon" "$appdir/bin/spartanport-probe" "$appdir/bin/gpu/libgportgpu.so" "$appdir/bin/gpu/libgportcpu.so" "$appdir/run.py"; do
    [[ -e $f ]] || die "package incomplete: $f"
done
while IFS= read -r -d '' shf; do
    head -n1 "$shf" | grep -q '/system/bin/sh' && die "host shebang found: $shf"
done < <(find "$appdir" -name '*.sh' -print0)
for bad in libvulkan_freedreno freedreno_icd liblsfg VkLayer_LS eboot.bin eboot.elf boot.bin; do
    if find "$appdir" -name "*$bad*" | grep -q .; then die "forbidden file in package: $bad"; fi
done

# verb 冒烟（无设备：config/info 走 python，stdout 必须是合法 JSON）
home_dir=$out/pkg/home/$program
export SPARTAN_HOME=$home_dir SPARTAN_DATA_DIR=$home_dir FXD_ROOT=/nonexistent
mkdir -p "$SPARTAN_DATA_DIR" "$SPARTAN_HOME"
( cd "$out/pkg" && sh "./$program/entry.sh" config schema | python3 -m json.tool >/dev/null \
  && sh "./$program/entry.sh" config get | python3 -m json.tool >/dev/null \
  && sh "./$program/entry.sh" info patches | python3 -m json.tool >/dev/null )
echo "    verb smoke OK"

# 宿主仓的 config.sh 用组件元数据（build/meta.json）算 rootfs 的 glibc 主版本
# （NXP_MAX_GLIBC，Port 的符号天花板）；那份元数据不在端口仓这边 ⇒ 用 sysroot 的 glibc
# 版本（同源：Debian trixie = rootfs 的那一份）。
if [[ -z ${NXP_MAX_GLIBC:-} ]]; then
    _major=$(sed -n 's/^#define[[:space:]]*__GLIBC__[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$sysroot/usr/include/features.h" 2>/dev/null | head -1)
    _minor=$(sed -n 's/^#define[[:space:]]*__GLIBC_MINOR__[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$sysroot/usr/include/features.h" 2>/dev/null | head -1)
    [[ -n $_major ]] && export NXP_MAX_GLIBC="$_major.${_minor:-0}"
fi
# AppImage 组装：宿主管线（glibc 天花板 + 排除清单 + mksquashfs + runtime + 魔数）。
# macOS 无 binutils 的 readelf —— 用 brew llvm 的 llvm-readelf 垫一层。
# 宿主仓（nixport）里的 AppImage 组包脚本：默认找同级目录 ../fexdroid，CI 里用
# SPARTAN_HOST_TOOLS 指到别处（runner 上同级目录可能在工作区之外）。
fexdroid_tools=${SPARTAN_HOST_TOOLS:-$root/../fexdroid}
[[ -n $fexdroid_tools && -x $fexdroid_tools/scripts/build-port-appimage.sh ]] || die "fexdroid/scripts/build-port-appimage.sh not found (../fexdroid)"
shim=$out/.readelf-shim; mkdir -p "$shim"
readelf_src=$(command -v readelf || { for c in "$(brew --prefix llvm 2>/dev/null)/bin/llvm-readelf"; do [[ -x $c ]] && echo "$c" && break; done; })
[[ -n $readelf_src ]] || die "readelf not found (brew install llvm or binutils)"
ln -sf "$readelf_src" "$shim/readelf"
mkdir -p "$dist"
appimage_out=$out/pkg/SpartanPort-arm64.AppImage
# 绝对路径：宿主脚本会先 cd 到它自己的仓库根，相对路径会找不到。
appdir_abs=$(cd "$appdir" && pwd)
appimage_abs="$appimage_out"
PATH="$shim:$PATH" bash "$fexdroid_tools/scripts/build-port-appimage.sh" "$appdir_abs" "$appimage_abs"

# Port 分发包 = .zip：包根含 port.json、恰好一个 .AppImage，以及可选的虚拟按键布局
# （Port打包规范.md §2/§3.2/§6.1；归档形态只有 .zip 一种——tar/tar.gz 不再是导入通道，
# 宿主选中即报错）。
# 不用 shell 的 zip：Python 的 zipfile 写出的条目名不带 macOS 的 ._ 垃圾，条目数也能断言。
virtualkeys=packaging/arm-linux/virtualkeys.json
python3 - "$dist/spartanport-port.zip" "$out/pkg/port.json" "$out/pkg/SpartanPort-arm64.AppImage" "$virtualkeys" <<'PY'
import os
import sys
import zipfile

target, manifest, appimage, virtualkeys = sys.argv[1:5]
with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
    archive.write(manifest, 'port.json')
    archive.write(appimage, 'SpartanPort-arm64.AppImage')
    # 虚拟按键布局（可选）：manifest 的 virtualKeys 指向它，宿主导入时物化成每游戏默认布局。
    if os.path.isfile(virtualkeys):
        archive.write(virtualkeys, os.path.basename(virtualkeys))
PY
# 验收：包根恰含 manifest + 一个 AppImage（+ 可选的虚拟按键布局）。
python3 - "$dist/spartanport-port.zip" <<'PY'
import sys
import zipfile

names = sorted(zipfile.ZipFile(sys.argv[1]).namelist())
expected = ['SpartanPort-arm64.AppImage', 'port.json']
extra = [n for n in names if n not in expected]
if names[:2] != expected or any(n not in expected and n != 'virtualkeys.json' for n in names):
    raise SystemExit(f'port package must contain exactly {expected} (plus virtualkeys.json), got {names}')
PY
end_stage "$dist/spartanport-port.zip ($(du -m "$dist/spartanport-port.zip" | cut -f1) MB)"

printf '\nPort package ready: %s\n' "$dist/spartanport-port.zip"
