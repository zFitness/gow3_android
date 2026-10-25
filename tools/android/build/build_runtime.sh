#!/usr/bin/env bash
# Build the game-free ARM64 runtime program tree for the fexdroid Runtime package.
#
# The android-glibc line's build_android_runtime.sh, reduced to the stages the fexdroid
# runtime model needs (对比清单迁移线的 D5/D6 决策)：
#   stages 4-6 (Turnip main/gen8, LSFG)  → 砍：Vulkan 驱动归 fexdroid rootfs（D5），
#                                          帧生成归 fexdroid x11-frame-generation
#   stage 7 (build_release_runtime.py)   → 换成 runtime/pack.py（runtime.json 包，任务 7.3）
#
# Usage: tools/android/build/build_runtime.sh
#   前置（host 状态，见 docs）：
#     .local-deps/android/arm64-sysroot   tools/android/fetch_arm64_sysroot.py 产出
#     .local-deps/root/usr                Boost/tsl-robin-map/VMA/glslang（可复用
#                                         gow_android_glibc 同名目录，自动回退）
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
root=$PWD
sysroot=${GPORT_ARM64_SYSROOT:-$root/.local-deps/android/arm64-sysroot}
headers=${GPORT_ARM64_HEADERS:-}
if [[ -z $headers && -d $root/../gow_android_glibc/.local-deps/root/usr ]]; then
    headers=$root/../gow_android_glibc/.local-deps/root/usr
    echo ">> GPORT_ARM64_HEADERS unset, reusing $headers"
fi
headers=${headers:-$root/.local-deps/root/usr}
# 解释器路径按目标宿主（fexdroid）在链接期烤进 probe；打包层 rewrite_interpreter 兜底。
rootfs=${GPORT_ROOTFS:-/data/user/0/ro.cobrabm.fexdroid/files/rootfs}
device_rootfs=${GPORT_RUNTIME_ROOTFS:-}
if [[ -z $device_rootfs ]]; then
    for cand in "$root/.local-deps/android/migration/x/rootfs" \
                "$root/../gow_android_glibc/.local-deps/android/migration/x/rootfs"; do
        [[ -d $cand ]] && { device_rootfs=$cand; break; }
    done
fi
export GPORT_ARM64_SYSROOT=$sysroot
export GPORT_ARM64_HEADERS=$headers
export GPORT_ROOTFS=$rootfs
export PATH="$headers/bin:$PATH"

die() { echo "build_runtime: $*" >&2; exit 1; }
[[ -d $sysroot/usr/lib/aarch64-linux-gnu ]] || die "sysroot missing ($root/tools/android/fetch_arm64_sysroot.py): $sysroot"
[[ -d $headers/share/cmake/tsl-robin-map ]] || die "host headers missing (Boost/robin-map/VMA/glslang): $headers"
[[ -d $device_rootfs/usr/lib/aarch64-linux-gnu ]] || die "device rootfs (device-libs source) missing: $device_rootfs"
command -v clang >/dev/null || die 'clang is required'
command -v llvm-readelf >/dev/null || die 'llvm-readelf is required'

step=0; _t0=0
begin() { step=$((step + 1)); _t0=$(date +%s); printf '\n==> [%d/3] %s\n' "$step" "$*"; }
end_stage() { printf '    %s (%dm%02ds)\n' "$*" $(( ($(date +%s) - _t0) / 60 )) $(( ($(date +%s) - _t0) % 60 )); }

begin "ARM64 native components (build_arm64.sh)"
bash tools/android/build/build_arm64.sh
end_stage "libgportcpu.so / libgportgpu.so / gport-probe"

begin "device-libs + kgsl-chip-id"
device_libs=${GPORT_DEVICE_LIBS:-$root/out/arm64/device-libs.txt}
mkdir -p "$(dirname -- "$device_libs")"
find "$device_rootfs/usr/lib/aarch64-linux-gnu" -type f -print | sed 's|.*/||' | sort -u > "$device_libs"
clang --target=aarch64-linux-gnu --sysroot="$sysroot" -O2 -fuse-ld=lld \
    tools/android/build/kgsl_chip_id.c -o out/arm64/kgsl-chip-id
end_stage "device-libs.txt / kgsl-chip-id"

begin "ARM64 bundle"
python3 tools/android/build_arm64_bundle.py --device-libs "$device_libs" --sysroot "$sysroot"
end_stage "out/arm64/bundle/"

printf '\nProgram tree ready: %s\nNext: python3 runtime/pack.py  (runtime.json 包，含验收清单)\n' "$root/out/arm64/bundle/"
