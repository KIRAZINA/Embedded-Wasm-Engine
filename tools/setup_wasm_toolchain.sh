#!/bin/sh
# tools/setup_wasm_toolchain.sh -- obtain a WebAssembly toolchain without
# root (no sudo, no package manager install).
#
# Downloads the Ubuntu packages for lld (wasm-ld), wabt (wasm2wat,
# wasm-objdump, wasm-validate) and binaryen (wasm-opt), unpacks them into
# a local prefix and prints the PATH fragment to use.
#
#   sh tools/setup_wasm_toolchain.sh [PREFIX]
#
# Default prefix: $HOME/tools/root. The script only needs `apt-get
# download` (no root) and `dpkg-deb -x`.
#
# When the packages are already installed system-wide (CI images with
# `apt-get install lld wabt`), this script is not needed: the tools are
# then on PATH already.

set -u

PREFIX="${1:-$HOME/tools/root}"
WORK="$(mktemp -d 2>/dev/null || echo /tmp/wasm_toolchain.$$)"
mkdir -p "$WORK" "$PREFIX" || exit 1

echo "toolchain prefix: $PREFIX"

have() {
    command -v "$1" >/dev/null 2>&1
}

need_pkgs=""
have wasm-ld || need_pkgs="$need_pkgs lld-18"
have wasm2wat || need_pkgs="$need_pkgs wabt"
have wasm-opt || need_pkgs="$need_pkgs binaryen"

if [ -z "$need_pkgs" ]; then
    echo "all tools already on PATH"
    exit 0
fi

cd "$WORK" || exit 1
for p in $need_pkgs; do
    echo "downloading $p"
    apt-get download "$p" || exit 1
done
for d in *.deb; do
    dpkg-deb -x "./$d" "$PREFIX" || exit 1
done

echo
echo "add these to PATH:"
echo "  export PATH=\"$PREFIX/usr/bin:$PREFIX/usr/lib/llvm-18/bin:\$PATH\""
echo
echo "verify with:"
echo "  wasm-ld --version; wasm2wat --version; wasm-opt --version"
