#!/usr/bin/env bash
# launcher-rust —— 交叉编译 ARM64 Linux 版启动器（gport 移植线专用）。
#
# 上游只在 Windows / x86_64 Linux 构建本启动器；本脚本用 macOS/Linux 主机 +
# 仓库的 Debian trixie arm64 sysroot（.local-deps/arm64-sysroot，缺了先跑
# packaging/arm-linux/fetch-sysroot.py）交叉出 aarch64 版。
#
# 依赖：
#   rustup target add aarch64-unknown-linux-gnu
#   clang + lld（brew llvm 或系统 llvm）
#   中国网络建议配 rsproxy（~/.cargo/config.toml + RUSTUP_DIST_SERVER）
# rfd 的文件选择框走 xdg-portal（Cargo.toml 里已改，纯 Rust，免 GTK）。
#
# 用法：bash launcher-rust/build-aarch64.sh
# 产物：launcher-rust/target/aarch64-unknown-linux-gnu/release/spartanport-launcher
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/.." && pwd)
cd "$here"   # cargo 要在 launcher-rust/ 里跑（Cargo.toml 在这）
sysroot=${SPARTAN_ARM64_SYSROOT:-$root/.local-deps/arm64-sysroot}
[[ -d $sysroot/usr/lib/aarch64-linux-gnu ]] || { echo "sysroot missing: $sysroot" >&2; exit 1; }

llvm_bin=${SPARTAN_LLVM_BIN:-}
for cand in "$(brew --prefix llvm 2>/dev/null || true)/bin" /usr/lib/llvm-*/bin; do
    [[ -x $cand/clang && -x $cand/llvm-strip ]] && { llvm_bin=$cand; break; }
done
[[ -n ${llvm_bin:-} ]] || { echo 'need clang + llvm-strip (brew install llvm)' >&2; exit 1; }
export PATH="$llvm_bin:$PATH"

# 覆盖 rust-toolchain.toml 的钉子：本机已有的稳定工具链即可（项目要求 >=1.87）。
export RUSTUP_TOOLCHAIN=${RUSTUP_TOOLCHAIN:-stable}
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=clang
export RUSTFLAGS="-C link-arg=--target=aarch64-linux-gnu -C link-arg=--sysroot=$sysroot -C link-arg=-fuse-ld=lld -C link-arg=-L$sysroot/usr/lib/aarch64-linux-gnu"
export CC_aarch64_unknown_linux_gnu=clang AR_aarch64_unknown_linux_gnu=llvm-ar
export CFLAGS_aarch64_unknown_linux_gnu="--sysroot=$sysroot"

cargo build --release --target aarch64-unknown-linux-gnu --bin spartanport-launcher
"$llvm_bin/llvm-strip" --strip-unneeded \
    "target/aarch64-unknown-linux-gnu/release/spartanport-launcher"
ls -la "target/aarch64-unknown-linux-gnu/release/spartanport-launcher"
