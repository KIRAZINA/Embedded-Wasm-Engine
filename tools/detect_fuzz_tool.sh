#!/bin/sh
# tools/detect_fuzz_tool.sh -- pick the fuzzing backend for this host.
#
# Detection order (first match wins):
#   1. libfuzzer : clang with -fsanitize=fuzzer (plus asan/ubsan)
#   2. afl       : AFL++ (afl-clang-fast)
#   3. loop      : plain deterministic seeded loop driver (always available)
#
# Prints exactly one of: libfuzzer, afl, loop

set -u

probe_dir="${TMPDIR:-/tmp}/wasm_fuzz_probe.$$"
mkdir -p "$probe_dir" 2>/dev/null || exit 3

cat > "$probe_dir/probe.c" <<'EOF'
#include <stddef.h>
#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
    return 0;
}
EOF

if command -v clang >/dev/null 2>&1; then
    if clang -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
        "$probe_dir/probe.c" -o "$probe_dir/probe" >/dev/null 2>&1; then
        rm -rf "$probe_dir"
        echo libfuzzer
        exit 0
    fi
fi

if command -v afl-clang-fast >/dev/null 2>&1; then
    rm -rf "$probe_dir"
    echo afl
    exit 0
fi

rm -rf "$probe_dir"
echo loop
exit 0
