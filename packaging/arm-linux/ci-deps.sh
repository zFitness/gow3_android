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
    glslang-tools libboost-dev

# ---- 宿主头文件：tsl-robin-map 1.4.1、VulkanMemoryAllocator 3.4.0 ------------------------
# 两者都是纯头文件库，各自带 CMake 安装规则，装到 $prefix 后与本地布局一致
# （include/tsl、include/vk_mem_alloc.h、share/cmake/<name>）。
mkdir -p "$prefix" "$root/.local-deps/src"
fetch() {  # $1=url $2=目录名 $3=tag
    local dir="$root/.local-deps/src/$2"
    if [[ ! -d $dir/.git ]]; then
        rm -rf "$dir"
        git clone --depth 1 --branch "$3" "$1" "$dir"
    fi
}
if [[ ! -f $prefix/include/tsl/robin_map.h ]]; then
    fetch https://github.com/shadps4/tsl-robin-map.git tsl-robin-map v1.4.1
    cmake -S "$root/.local-deps/src/tsl-robin-map" -B "$root/.local-deps/src/tsl-robin-map/build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null
    cmake --install "$root/.local-deps/src/tsl-robin-map/build" >/dev/null
fi
if [[ ! -f $prefix/include/vk_mem_alloc.h ]]; then
    fetch https://github.com/NVlabs/VulkanMemoryAllocator.git VulkanMemoryAllocator v3.4.0
    cmake -S "$root/.local-deps/src/VulkanMemoryAllocator" -B "$root/.local-deps/src/VulkanMemoryAllocator/build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null
    cmake --install "$root/.local-deps/src/VulkanMemoryAllocator/build" >/dev/null
fi
[[ -d $prefix/share/cmake/tsl-robin-map ]] || die "tsl-robin-map cmake config missing"
[[ -d $prefix/share/cmake/VulkanMemoryAllocator ]] || die "VulkanMemoryAllocator cmake config missing"

# ---- Boost 的 cmake 目录（build.sh 用 Boost_DIR 找它）---------------------------------
boost_dir=$(find /usr/lib /usr/local/lib -maxdepth 3 -type d -name 'Boost-*' -path '*/cmake/*' 2>/dev/null | sort | tail -n 1)
[[ -n $boost_dir ]] || die "boost cmake dir not found (apt libboost-dev)"
echo "SPARTAN_BOOST_DIR=$boost_dir"

echo "ci-deps: ready (headers: $prefix)"
