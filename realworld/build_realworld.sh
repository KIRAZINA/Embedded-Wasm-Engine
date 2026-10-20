#!/bin/sh
# realworld/build_realworld.sh — build the real-world readiness programs
# twice (native and WebAssembly) and prove the outputs match.
#
#   sh realworld/build_realworld.sh [OUTDIR]
#
# Steps per program:
#   1. native build with the host compiler  -> expected output line
#   2. wasm build with clang --target=wasm32 -> module
#   3. MVP-only check of the emitted opcodes (no sign extension, no bulk
#      memory, no non-trapping conversions, no multi-value, no reference
#      types, no SIMD, no mutable globals, no imports)
#   4. run the module through build/wasm_runner and compare the output
#      with the native one, byte for byte
#
# The wasm modules are also copied into realworld/ so that `make test`
# can execute the committed artifacts without a wasm toolchain.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/realworld}
PROGRAMS="qsort_demo matmul_f32 crc32_demo"

# Toolchain: wasm-ld (lld) and the wabt tools. They are found on PATH or
# in the local prefix created by tools/setup_wasm_toolchain.sh.
if ! command -v wasm-ld >/dev/null 2>&1; then
    for prefix in "$HOME/tools/root/usr/lib/llvm-18/bin" "$HOME/tools/root/usr/bin"; do
        if [ -x "$prefix/wasm-ld" ] || [ -x "$prefix/wasm-ld-18" ]; then
            PATH="$prefix:$PATH"
            export PATH
        fi
    done
fi

CLANG=${CLANG:-clang}
CC_HOST=${CC_HOST:-cc}

# MVP-only feature set: everything the engine does not implement (or that
# is not part of the MVP) is switched off explicitly.
WASM_FEATURES="-mno-sign-ext -mno-bulk-memory -mno-mutable-globals \
-mno-reference-types -mno-nontrapping-fptoint -mno-multivalue \
-mno-extended-const -mno-simd128 -mno-tail-call -mno-exception-handling"

WASM_CFLAGS="--target=wasm32 -nostdlib -O2 -ffreestanding -fno-builtin \
-fno-stack-protector -fno-lto $WASM_FEATURES"

# -z stack-size keeps the shadow stack small so the module's data starts
# inside the engine's one-page CLI arena.
WASM_LDFLAGS="-Wl,--no-entry -Wl,-z,stack-size=8192 -Wl,--strip-all"

# Opcodes and constructs the engine must never see. The pattern is one
# line so that no shell line continuation can break it.
FORBIDDEN_PATTERN='i32\.extend8_s|i32\.extend16_s|i64\.extend8_s|i64\.extend16_s|memory\.copy|memory\.fill|memory\.init|data\.drop|table\.init|table\.copy|table\.fill|table\.get|table\.set|table\.grow|table\.size|elem\.drop|i32\.trunc_sat|i64\.trunc_sat|ref\.null|ref\.func|ref\.is_null|call_ref|return_call|v128\.|\(global \(mut'

fail() {
    echo "realworld: FAIL: $*" >&2
    exit 1
}

command -v "$CLANG" >/dev/null 2>&1 || fail "clang not found"
command -v wasm-ld >/dev/null 2>&1 || fail "wasm-ld not found (see tools/setup_wasm_toolchain.sh)"
[ -x "$ROOT/build/wasm_runner" ] || [ -x "$ROOT/build/wasm_runner.exe" ] || \
    fail "build/wasm_runner missing (run make all first)"
if command -v wasm2wat >/dev/null 2>&1; then
    HAVE_WAT=1
else
    HAVE_WAT=0
fi

mkdir -p "$OUT" || fail "cannot create $OUT"
RUNNER="$ROOT/build/wasm_runner"
[ -x "$RUNNER" ] || RUNNER="$ROOT/build/wasm_runner.exe"

status=0
for prog in $PROGRAMS; do
    src="$ROOT/realworld/$prog.c"
    [ -f "$src" ] || fail "missing $src"

    echo "--- $prog"

    # 1. native reference
    "$CC_HOST" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -DNATIVE_BUILD \
        -I"$ROOT/realworld" "$src" -o "$OUT/${prog}_native" -lm || \
        fail "$prog: native build failed"
    "$OUT/${prog}_native" > "$OUT/${prog}.native.txt" || fail "$prog: native run failed"
    expected=$(cat "$OUT/${prog}.native.txt")
    echo "    native : $expected"

    # 2. wasm build
    "$CLANG" $WASM_CFLAGS -I"$ROOT/realworld" "$src" $WASM_LDFLAGS \
        -o "$OUT/${prog}.wasm" || fail "$prog: wasm build failed"
    size=$(wc -c < "$OUT/${prog}.wasm")
    echo "    wasm   : $size bytes"

    # 3. MVP-only check
    if [ "$HAVE_WAT" = "1" ]; then
        wasm2wat "$OUT/${prog}.wasm" > "$OUT/${prog}.wat" || \
            fail "$prog: wasm2wat failed"
        if grep -Eq "$FORBIDDEN_PATTERN" "$OUT/${prog}.wat"; then
            echo "    post-MVP construct found:" >&2
            grep -En "$FORBIDDEN_PATTERN" "$OUT/${prog}.wat" >&2
            fail "$prog: emitted post-MVP features"
        fi
        # multi-value: a function type with more than one result
        if grep -Eq '\(result [^)]+ [^)]+\)' "$OUT/${prog}.wat"; then
            fail "$prog: multi-value function type emitted"
        fi
        if grep -Eq '^\s*\(import' "$OUT/${prog}.wat"; then
            fail "$prog: module imports something"
        fi
        echo "    check  : MVP-only opcodes, no imports"
    else
        echo "    check  : skipped (wasm2wat not available)"
    fi

    # 4. run through the engine and compare
    "$RUNNER" --quiet "$OUT/${prog}.wasm" main > "$OUT/${prog}.wasm.txt" || \
        fail "$prog: wasm_runner failed"
    got=$(cat "$OUT/${prog}.wasm.txt")
    echo "    engine : $got"
    if [ "$expected" != "$got" ]; then
        echo "    MISMATCH: native '$expected' vs engine '$got'" >&2
        status=1
        continue
    fi
    echo "    match  : OK"

    # keep the module next to the sources so `make test` can run it
    cp "$OUT/${prog}.wasm" "$ROOT/realworld/${prog}.wasm" || fail "copy failed"
done

if [ "$status" -ne 0 ]; then
    fail "native and engine outputs differ"
fi
echo "realworld: all programs match"
exit 0
