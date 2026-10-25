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
    python3 packaging/arm-linux/fetch-sysroot.py --out "$sysroot"
fi
headers=${SPARTAN_ARM64_HEADERS:-}
if [[ -z $headers && -d $root/../gow_android_glibc/.local-deps/root/usr ]]; then
    headers=$root/../gow_android_glibc/.local-deps/root/usr
fi
headers=${headers:-$root/.local-deps/root/usr}
[[ -d $headers/share/cmake/tsl-robin-map ]] || die "host headers missing (tsl-robin-map/VMA/glslang): $headers"
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
if [[ ! -f $out/gpu/build.ninja ]]; then
    cmake -S gpu -B "$out/gpu" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$tc" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSPARTAN_PGO=off -DSPARTAN_LTO=OFF \
        -DSPARTAN_ARM64_HEADER_PREFIX="$headers" \
        -Dtsl-robin-map_DIR="$headers/share/cmake/tsl-robin-map" \
        -DVulkanMemoryAllocator_DIR="$headers/share/cmake/VulkanMemoryAllocator" \
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
find "$out/lib" -name '*.so*' -exec "$llvm_bin/llvm-strip" --strip-unneeded {} + 2>/dev/null || true
end_stage "$(ls "$out/lib" | wc -l | tr -d ' ') libs"

# ---- 5/5 程序树组装 + fexdroid Runtime 包 ---------------------------------------------
begin "package (fexdroid runtime)"
stage=$out/pkg/$program
rm -rf "$out/pkg"
mkdir -p "$stage/bin/gpu" "$stage/lib" "$stage/config"
install -m755 "$out/bin/spartanport-probe" "$stage/bin/"
install -m755 "$out/bin/gpu/libgportgpu.so" "$out/bin/gpu/libgportcpu.so" "$stage/bin/gpu/"
cp -R "$out/lib/." "$stage/lib/"
install -m755 packaging/arm-linux/entry.sh "$stage/entry.sh"
install -m644 packaging/arm-linux/entry_main.py "$stage/entry_main.py"
install -m644 run.py "$stage/"
cp -R scripts "$stage/scripts"
find "$stage/scripts" -name __pycache__ -prune -exec rm -r {} + 2>/dev/null || true
cp -R patches "$stage/patches"
cp -R launcher "$stage/launcher"
find "$stage/launcher" -name __pycache__ -prune -exec rm -r {} + 2>/dev/null || true
for name in gport-fex-compatible.json gport-gamecontrollerdb.txt gport-packages.txt; do
    [[ -f packaging/arm-linux/config/$name ]] && install -m644 "packaging/arm-linux/config/$name" "$stage/config/"
done
install -m644 packaging/arm-linux/runtime.json "$stage/../runtime.json"
mkdir -p "$stage/../assets"
[[ -f packaging/arm-linux/assets/cover.png ]] && install -m644 packaging/arm-linux/assets/cover.png "$stage/../assets/cover.png"

# 验收：manifest / 布局 / 黑名单（驱动与游戏数据不得入包）
[[ $(python3 -c "import json;print(json.load(open('$stage/../runtime.json'))['schema'])") == fexdroid.runtime.v1 ]] || die 'runtime.json schema wrong'
for f in "$stage/entry.sh" "$stage/bin/spartanport-probe" "$stage/bin/gpu/libgportgpu.so" "$stage/bin/gpu/libgportcpu.so" "$stage/run.py"; do
    [[ -e $f ]] || die "package incomplete: $f"
done
for bad in libvulkan_freedreno freedreno_icd liblsfg VkLayer_LS eboot.bin eboot.elf boot.bin; do
    if find "$stage" -name "*$bad*" | grep -q .; then die "forbidden file in package: $bad"; fi
done

# verb 冒烟（无设备：config/info 走 python，stdout 必须是合法 JSON）
export SPARTAN_DATA_DIR=$out/pkg/gdata SPARTAN_HOME=$out/pkg/home/$program FXD_ROOT=/nonexistent
mkdir -p "$SPARTAN_DATA_DIR" "$SPARTAN_HOME"
( cd "$out/pkg" && sh "./$program/entry.sh" config schema | python3 -m json.tool >/dev/null \
  && sh "./$program/entry.sh" config get | python3 -m json.tool >/dev/null \
  && sh "./$program/entry.sh" info patches | python3 -m json.tool >/dev/null )
echo "    verb smoke OK"

mkdir -p "$dist"
( cd "$out/pkg" && tar czf "$dist/spartanport-runtime.tar.gz" runtime.json assets "$program" )
end_stage "$dist/spartanport-runtime.tar.gz ($(du -m "$dist/spartanport-runtime.tar.gz" | cut -f1) MB)"

printf '\nRuntime package ready: %s\n' "$dist/spartanport-runtime.tar.gz"
