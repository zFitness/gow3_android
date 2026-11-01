#!/usr/bin/env bash
# packaging/arm-linux — CI 依赖：交叉工具链、宿主头文件（tsl-robin-map / VMA / glslang）、Boost。
#
# 与本地构建的差别只有两处：本地用 brew 装的 llvm/boost 和手放的宿主头文件，这里用 apt
# 与源码拉取。产物落在 .local-deps/root/usr（SPARTAN_ARM64_HEADERS 指向它）。
#
# 用法：bash packaging/arm-linux/ci-deps.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
prefix=${SPARTAN_ARM64_HEADERS:-$root/.local-deps/root/usr}

die() { echo "ci-deps: $*" >&2; exit 1; }

# ---- 宿主包（Ubuntu 24.04 的名字）------------------------------------------------------
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    clang lld llvm cmake ninja-build pkg-config git curl ca-certificates \
    python3 python3-pip zip unzip file squashfs-tools \
    glslang-tools libboost-dev robin-map-dev

# ---- 宿主头文件：tsl-robin-map（apt 的 robin-map-dev 就是它）、VulkanMemoryAllocator 3.4.0 ---
# tsl-robin-map 是纯头文件库，Ubuntu 的 robin-map-dev 装的就是它（cmake 包名 tsl-robin-map，
# 头文件在 /usr/include/tsl），不必从源码拉。
# VulkanMemoryAllocator 同理是纯头文件库，GitHub 上没有发行包 ⇒ 拉源码用它的 CMake 安装规则装到
# $prefix（include/vk_mem_alloc.h + share/cmake/VulkanMemoryAllocator）。
mkdir -p "$prefix" "$root/.local-deps/src"
if [[ ! -f $prefix/include/vk_mem_alloc.h ]]; then
    dir="$root/.local-deps/src/VulkanMemoryAllocator"
    if [[ ! -d $dir/.git ]]; then
        rm -rf "$dir"
        git clone --depth 1 --branch v3.4.0 https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git "$dir"
    fi
    cmake -S "$dir" -B "$dir/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null
    cmake --install "$dir/build" >/dev/null
fi
tsl_dir=$(find /usr/lib /usr/local/lib -maxdepth 4 -type d -name 'tsl-robin-map' -path '*cmake*' 2>/dev/null | head -n 1)
[[ -n $tsl_dir ]] || die "tsl-robin-map cmake config not found (apt robin-map-dev)"
echo "tsl-robin-map: $tsl_dir"

# ---- Boost 的 cmake 目录（build.sh 用 Boost_DIR 找它）---------------------------------
boost_dir=$(find /usr/lib /usr/local/lib -maxdepth 3 -type d -name 'Boost-*' -path '*/cmake/*' 2>/dev/null | sort | tail -n 1)
[[ -n $boost_dir ]] || die "boost cmake dir not found (apt libboost-dev)"
echo "SPARTAN_BOOST_DIR=$boost_dir"

echo "ci-deps: ready (headers: $prefix)"
