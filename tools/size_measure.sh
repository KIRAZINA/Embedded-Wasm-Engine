#!/bin/sh
# tools/size_measure.sh — measure the engine's static footprint in a
# typical embedded configuration (-Os, freestanding, Cortex-M flags for
# the sizes, host toolchain for the measurement).
#
# Prints one line per metric:
#   text=<bytes> bss=<bytes> data=<bytes> total=<bytes>
#
# The engine objects are measured without libc, so the numbers only move
# when the engine's own code changes. The committed size_baseline.txt
# holds the reference values; tools/check_size_baseline.py fails when
# .text or .bss grew by more than 5 %.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/size"
CC_TOOL=${CC:-cc}
SIZE_TOOL=${SIZE:-size}

mkdir -p "$OUT" || exit 1

SOURCES="leb128 wasm_parser wasm_stack wasm_memory wasm_frame wasm_control wasm_vm wasm_host wasm_validate"

for s in $SOURCES; do
    "$CC_TOOL" -std=c11 -Wall -Wextra -Wpedantic -Werror -I"$ROOT/include" \
        -Os -ffreestanding -fno-builtin -c "$ROOT/src/$s.c" -o "$OUT/$s.o" || exit 1
done

if ! command -v "$SIZE_TOOL" >/dev/null 2>&1; then
    echo "ERROR: '$SIZE_TOOL' not found (install binutils)" >&2
    exit 1
fi

TOTALS=$("$SIZE_TOOL" -t "$OUT"/*.o | tail -1 | awk '{print $1, $2, $3}')
TEXT=$(echo "$TOTALS" | awk '{print $1}')
DATA=$(echo "$TOTALS" | awk '{print $2}')
BSS=$(echo "$TOTALS" | awk '{print $3}')
TOTAL=$(echo "$TOTALS" | awk '{print $1 + $2 + $3}')
echo "text=$TEXT bss=$BSS data=$DATA total=$TOTAL"
