#!/usr/bin/env bash
# Build x86-64 loaders with generic CRT startup objects, even on CachyOS v4.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
baseline=${GPORT_THOR_BASELINE:-"$PWD/.local-deps/android/baseline"}
local_deps="$PWD/.local-deps/root/usr"
[[ -f $baseline/usr/lib/crt1.o ]] || { echo 'Fetch the generic libraries first.' >&2; exit 1; }
export CPATH="$local_deps/include${CPATH:+:$CPATH}"
export LIBRARY_PATH="$local_deps/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
mkdir -p out/thor
gpu=(-Lout/gpu -lgow3gpu -Wl,-rpath,"$PWD/out/gpu" -Wl,-rpath,"$baseline/usr/lib" -rdynamic)
"${CC:-gcc}" -B"$baseline/usr/lib/" -march=x86-64 -mtune=generic \
    -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie \
    "${includes[@]}" -I. -Isrc src/probe.c src/runtime*.c src/vulkan_smoke.c \
    out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/thor/gport-probe
common=(-std=c++23 -O2 -fPIC -DFMT_SHARED -Igpu/shim -Igpu/shadps4)
"${CXX:-g++}" "${common[@]}" -fvisibility=hidden -fvisibility-inlines-hidden \
    -c tools/android/build/renderer_probe.cpp -o out/thor/renderer_probe.o
"${CXX:-g++}" "${common[@]}" -c tests/test_gpu_runtime_stubs.cpp -o out/thor/runtime_stubs.o
"${CXX:-g++}" -B"$baseline/usr/lib/" -no-pie out/thor/renderer_probe.o \
    out/thor/runtime_stubs.o "${gpu[@]}" -o out/thor/renderer-probe
"${CC:-gcc}" -B"$baseline/usr/lib/" -march=x86-64 -O2 -Wall -Wextra -Werror \
    "${includes[@]}" tools/android/build/controller_probe.c "${libraries[@]}" -o out/thor/controller-probe
"${CC:-gcc}" -B"$baseline/usr/lib/" -march=x86-64 -O2 -Wall -Wextra -Werror \
    "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" -o out/thor/bb-gpu-capabilities
readelf -n out/thor/gport-probe out/thor/renderer-probe
