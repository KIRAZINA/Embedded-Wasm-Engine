#!/bin/sh
# Fetch + convert the official WebAssembly spec testsuite (reproducible).
#
# Usage: sh tools/fetch_spec_tests.sh [SPEC_REF] [WAST2JSON]
#   SPEC_REF   spec commit (default: pinned below; "HEAD" for a fresh pull)
#   WAST2JSON  converter binary (default: wast2json from PATH)
#
# Layout produced (all under tests/spec/, gitignored except DEVIATIONS.md):
#   upstream/   shallow spec checkout (test/core/*.wast)
#   json/       per-suite NAME.json + NAME.N.wasm (wast2json output)
#
# Requirements: git, wast2json (wabt; e.g. `apt install wabt` on Debian).
# Pinned commit and converter version are printed for the Phase 3 report.
set -eu

SPEC_REF="${1:-340650f2f620169685814774bc5b019d05d4d3d7}"
WAST2JSON="${2:-wast2json}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UPSTREAM="$ROOT/tests/spec/upstream"
OUT="$ROOT/tests/spec/json"

SUITES="address align block br br_if br_table call call_indirect const
conversions data elem endianness exports f32 f32_bitwise f32_cmp f64
f64_bitwise f64_cmp fac float_exprs float_literals float_memory float_misc
forward func func_ptrs global i32 i64 if imports int_exprs int_literals
labels left-to-right linking load local_get local_set local_tee loop memory
memory_grow memory_size memory_trap names nop return select start store
switch table traps type unreachable unreached-invalid unreached-valid unwind
utf8-custom-section-id utf8-import-field utf8-import-module
utf8-invalid-encoding"

command -v git >/dev/null || { echo "fetch: git not found" >&2; exit 1; }
command -v "$WAST2JSON" >/dev/null || {
  echo "fetch: $WAST2JSON not found (install wabt)" >&2; exit 1; }

if [ ! -d "$UPSTREAM/test/core" ]; then
  rm -rf "$UPSTREAM"
  git clone --depth 1 https://github.com/WebAssembly/spec "$UPSTREAM"
fi
if [ "$SPEC_REF" != "HEAD" ]; then
  git -C "$UPSTREAM" fetch --depth 1 origin "$SPEC_REF" 2>/dev/null || true
  git -C "$UPSTREAM" checkout --quiet "$SPEC_REF" 2>/dev/null || {
    echo "fetch: cannot resolve $SPEC_REF; staying on HEAD" >&2; }
fi
echo "fetch: spec @ $(git -C "$UPSTREAM" rev-parse HEAD)"
echo "fetch: $($WAST2JSON --version)"
mkdir -p "$OUT"

# wast2json enables most MVP-era features by default, but several listed
# suites (br_table, select, table, linking, ...) now contain
# reference-type/function-reference cases that only parse with everything
# on. --enable-all is content-identical for pure-MVP files (verified on
# nop.wast: only the output filename stem differs), so convert uniformly;
# post-MVP constructs become binaries the MVP engine rejects, and the spec
# harness classifies those asserts as known deviations per case.
WASTFLAGS="--enable-all"
count=0
missing=""
for suite in $SUITES; do
  src="$UPSTREAM/test/core/$suite.wast"
  if [ ! -f "$src" ]; then
    missing="$missing $suite"
    continue
  fi
  "$WAST2JSON" $WASTFLAGS "$src" -o "$OUT/$suite.json"
  count=$((count + 1))
done
echo "fetch: converted $count suites into $OUT"
if [ -n "$missing" ]; then
  echo "fetch: missing in this spec revision:$missing" >&2
fi
