#!/usr/bin/env bash
# Native ARM64 build for the device's Linux userland (Debian trixie arm64), cross-compiled with
# Clang: the runtime and the GPU library run natively, the game's x86-64 code runs in FEXCore
# (libgportcpu.so). The Android counterpart of build.sh's Linux branch.
#
# Inputs (see docs/ANDROID_PORT.md):
#   .local-deps/android/arm64-sysroot   Debian trixie arm64 sysroot (tools/android/fetch_arm64_sysroot.py)
#   .local-deps/root/usr                host header-only deps: Boost, tsl-robin-map, VMA, glslang
#   GPORT_FEX_SOURCE                     FEXCore source; defaults to third_party/FEX
#   gpu/third_party/{miniz,magic_enum,imgui,xbyak,fsr-vulkan,sirit}  submodules/vendored
set -Eeuo pipefail
# CI logs a bare "exit code 1" when a step dies without a diagnostic; name the failing command so
# the log points straight at it. -E (errtrace) keeps this working inside functions and subshells.
trap 'printf "build_arm64.sh: line %s failed: %s\n" "$LINENO" "$BASH_COMMAND" >&2' ERR
cd -- "$(dirname -- "$0")/../../.."
# A conda-forge gcc activation leaks host-only x86-64 flags (CFLAGS=-march=nocona -mtune=haswell,
# CPATH=$CONDA_PREFIX/include) that clang rejects for the aarch64 target. conda also sets
# CMAKE_PREFIX_PATH=$CONDA_PREFIX, which makes FindPkgConfig search the host's .pc files and return
# $CONDA_PREFIX/include paths that get sysroot-prefixed into a broken path. Header deps are passed
# explicitly via -D flags / the sysroot, so drop those env vars before any cross cmake/clang run.
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH \
    CMAKE_PREFIX_PATH
root=$PWD
out=$root/out/arm64
sysroot=${GPORT_ARM64_SYSROOT:-$root/.local-deps/android/arm64-sysroot}
headers=${GPORT_ARM64_HEADERS:-$root/.local-deps/root/usr}
fex_source=${GPORT_FEX_SOURCE:-$root/third_party/FEX}
boost_dir=${GPORT_BOOST_DIR:-$headers/lib/cmake/Boost-1.87.0}
toolchain=$root/tools/android/build/aarch64-toolchain.cmake
# The in-rootfs interpreter is baked into the probe at link time (Android has no /lib).
rootfs=${GPORT_ROOTFS:-/data/user/0/com.gow3.android/files/rootfs}
export PATH="$headers/bin:$PATH"   # glslangValidator for gpu/host_shaders

[[ -f $sysroot/usr/lib/aarch64-linux-gnu/libc.so.6 ]] || { echo "No sysroot: $sysroot" >&2; exit 1; }
[[ -d $headers/share/cmake/tsl-robin-map ]] || { echo "No host deps in $headers (Boost/robin-map/VMA/glslang)" >&2; exit 1; }
[[ -f $fex_source/CMakeLists.txt ]] || {
    echo "No FEX source: $fex_source (set GPORT_FEX_SOURCE or initialize third_party/FEX)" >&2
    exit 1
}
mkdir -p "$out"

# ---- pkg-config cross sanity check ------------------------------------------
# conda-forge's pkg-config (pkgconf) enables --define-prefix by default: it relocates the Debian
# "/usr" prefix inside the sysroot's .pc files and then prefixes PKG_CONFIG_SYSROOT_DIR on top of
# that, so CMake gets <sysroot>/<sysroot>/usr/lib/... and the generate step dies with
# 'Imported target "PkgConfig::XXHASH" includes non-existent path'. Resolve an invocation that
# returns real directories before FEX spends four minutes compiling.
export PKG_CONFIG_SYSROOT_DIR=$sysroot
export PKG_CONFIG_LIBDIR=$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/share/pkgconfig
export PKG_CONFIG_PATH=
export PKG_CONFIG_DONT_DEFINE_PREFIX=1
export PKG_CONFIG_FDO_SYSROOT_RULES=1
pc_flags=()
if pkg-config --dont-define-prefix --version >/dev/null 2>&1; then
    pc_flags=(--dont-define-prefix)
fi
pc_dirs_resolve() {
    local dir cflags
    cflags=$(pkg-config "${pc_flags[@]}" --cflags-only-I libxxhash libavcodec 2>/dev/null) || return 1
    [[ -n $cflags ]] || return 1
    while IFS= read -r dir; do
        [[ -d $dir ]] || return 1
    done < <(printf '%s\n' $cflags | tr ' ' '\n' | sed -n 's/^-I//p')
    return 0
}
if ! pc_dirs_resolve; then
    pc_flags=()
    if ! pc_dirs_resolve; then
        echo 'pkg-config cannot resolve the sysroot include directories.' >&2
        printf 'pkg-config: %s (%s)\n' "$(command -v pkg-config)" \
            "$(pkg-config --version 2>/dev/null)" >&2
        printf 'PKG_CONFIG_SYSROOT_DIR=%s\n' "$PKG_CONFIG_SYSROOT_DIR" >&2
        printf 'PKG_CONFIG_LIBDIR=%s\n' "$PKG_CONFIG_LIBDIR" >&2
        echo 'raw --cflags-only-I libxxhash libavcodec:' >&2
        pkg-config --cflags-only-I libxxhash libavcodec >&2 || true
        exit 1
    fi
fi
printf '    pkg-config: %s (%s), flags: %s\n' "$(command -v pkg-config)" \
    "$(pkg-config --version 2>/dev/null)" "${pc_flags[*]:-<none>}"
pkg-config "${pc_flags[@]}" --cflags-only-I libxxhash libavcodec 2>/dev/null |
    tr ' ' '\n' | sed -n 's/^-I/    include: /p'
unset PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_LIBDIR

# ---- progress helpers -------------------------------------------------------
# Banners/elapsed time print on every run; the rolling "[N/M]" line only shows on a TTY
# (no-op when output is piped, so CI logs stay clean).
step=0
total=4
_t0=0

begin() { step=$((step + 1)); _t0=$(date +%s); printf '\n==> [%d/%d] %s\n' "$step" "$total" "$*"; }

end_stage() {
    local sec=$(( $(date +%s) - _t0 ))
    printf '    %s (%dm%02ds)\n' "$*" "$((sec / 60))" "$((sec % 60))"
}

# Print a live status line on a TTY only. The `if` form matters: `[[ -t 1 ]] && printf` would
# return 1 when stdout is a pipe (CI), which `set -e` turns into a silent early exit.
live() { if [[ -t 1 ]]; then printf '\r    %s\033[K' "$*"; fi; }
live_end() { if [[ -t 1 ]]; then printf '\r\033[K\n'; fi; }

# Run ninja and return its exit status. On a TTY, run it in the background and poll the
# full log (kept in $3 for diagnostics) for the latest "[N/M] ..." line, shown as a rolling
# progress line. On non-TTY, run it in the foreground exactly as before.
build_ninja() {
    local dir=$1 target=$2 log=$3 pid line last="" status=0
    if [[ -t 1 ]]; then
        ninja -C "$dir" "$target" > "$log" 2>&1 &
        pid=$!
        trap 'kill "$pid" 2>/dev/null || true' INT TERM
        while kill -0 "$pid" 2>/dev/null; do
            line=$(grep -E '^\[[0-9]+/[0-9]+\]' "$log" 2>/dev/null | tail -1 || true)
            if [[ -n $line && $line != "$last" ]]; then
                live "${line:0:100}"
                last=$line
            fi
            sleep 2
        done
        wait "$pid" || status=$?
        trap - INT TERM
        if [[ -n $last ]]; then live_end; fi
        return "$status"
    fi
    ninja -C "$dir" "$target" > "$log" 2>&1 || status=$?
    return "$status"
}

# This port's changes to FSR-Vulkan (gpu/patches/fsr-vulkan), applied once, as build.sh does.
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$root/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$root/$patch"
    fi
done

# ---- 1/4 FEXCore + libgportcpu.so (the guest CPU) ----------------------------
begin "FEXCore + libgportcpu.so"
if [[ ! -f $out/fex/build.ninja ]]; then
    echo "    cmake: $fex_source"
    cmake -S "$fex_source" -B "$out/fex" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_FEX_INCLUDE="$root/cpu/fex_project.cmake" \
        -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_STEAM_SUPPORT=OFF \
        -DBUILD_FEX_LINUX_TESTS=OFF -DENABLE_ZYDIS=OFF -DTUNE_ARCH=armv8.2-a -DTUNE_CPU=generic >/dev/null
fi
if ! build_ninja "$out/fex" gportcpu "$out/fex-build.log"; then
    echo "    FEX build failed (full log: $out/fex-build.log)" >&2
    tail -40 "$out/fex-build.log" >&2
    exit 1
fi
end_stage "libgportcpu.so"

# ---- 2/4 libgportgpu.so (shadPS4 video core + drivers) -----------------------
begin "libgportgpu.so (shadPS4 video core)"
if [[ ! -f $out/gpu/build.ninja ]]; then
    echo "    cmake: gpu/"
    cmake -S gpu -B "$out/gpu" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGOW3_PGO=off -DGOW3_LTO=OFF \
        -DGOW3_ARM64_HEADER_PREFIX="$headers" \
        -Dtsl-robin-map_DIR="$headers/share/cmake/tsl-robin-map" \
        -DVulkanMemoryAllocator_DIR="$headers/share/cmake/VulkanMemoryAllocator" \
        -DBoost_DIR="$boost_dir" \
        -DZydis_DIR="$sysroot/usr/lib/aarch64-linux-gnu/cmake/zydis" >/dev/null
fi
if ! build_ninja "$out/gpu" spartanportgpu "$out/gpu-build.log"; then
    echo "    GPU build failed (full log: $out/gpu-build.log)" >&2
    grep -v '^\[' "$out/gpu-build.log" | tail -40 >&2
    exit 1
fi
end_stage "libgportgpu.so"

# ---- 3/4 gport-probe (the loader) --------------------------------------------
cc=(clang --target=aarch64-linux-gnu --sysroot="$sysroot" -fuse-ld=lld)
export PKG_CONFIG_SYSROOT_DIR=$sysroot
export PKG_CONFIG_LIBDIR=$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/share/pkgconfig
# See aarch64-toolchain.cmake: keep pkgconf's --define-prefix off so the sysroot is applied once.
export PKG_CONFIG_DONT_DEFINE_PREFIX=1
export PKG_CONFIG_FDO_SYSROOT_RULES=1
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3 libavcodec libavutil libswscale)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3 libavcodec libavutil libswscale)"

begin "LibAtrac9 (third-party decoders)"
mkdir -p "$out/atrac9"
atrac9_sources=(third_party/LibAtrac9/C/src/*.c)
atrac9_total=${#atrac9_sources[@]}
n=0
for source in "${atrac9_sources[@]}"; do
    n=$((n + 1))
    live "atrac9 $n/$atrac9_total  $(basename "$source")"
    "${cc[@]}" -std=c99 -O2 -w -c "$source" -o "$out/atrac9/$(basename "${source%.c}").o"
done
live_end
archiver=${GPORT_AR:-$(command -v llvm-ar || true)}
[[ -n $archiver ]] || { echo "No LLVM archiver (set GPORT_AR)" >&2; exit 1; }
"$archiver" rcs "$out/libatrac9.a" "$out"/atrac9/*.o
end_stage "libatrac9.a"

# ---- 4/4 gport-probe (the loader) --------------------------------------------
begin "gport-probe (loader)"
# src/runtime*.c includes runtime_cpu.c (a stub on ARM64: libgportcpu.so implements guest_cpu.h).
"${cc[@]}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc \
    src/probe.c src/runtime*.c src/vulkan_smoke.c "$out/libatrac9.a" -lm \
    -L"$out/gpu" -lgportgpu -L"$out/fex" -lgportcpu -Wl,-rpath,'$ORIGIN/../lib' \
    -Wl,--dynamic-linker="$rootfs/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" -rdynamic \
    "${libraries[@]}" -o "$out/gport-probe"
end_stage "gport-probe"

printf '\nAll ARM64 artifacts built:\n'
ls -lh "$out/fex/libgportcpu.so" "$out/gpu/libgportgpu.so" "$out/gport-probe"
