#!/usr/bin/env bash
# fetch_spec_tests.sh — reproduce official WebAssembly spec corpus
# Invariant: zero C allocation; this is Python/shell tooling only.
#
# POSIX sh compatible on purpose: CI and the release verification invoke
# it as `sh tools/fetch_spec_tests.sh`, and `set -o pipefail` is not
# available there.
set -eu

UPSTREAM="tests/spec/upstream"
OUTDIR="tests/spec/json"

# 1. Obtain upstream spec repo (shallow, only test/core/*.wast needed)
if [ ! -d "$UPSTREAM/test/core" ]; then
    echo "Cloning spec repo (shallow) ..."
    rm -rf "$UPSTREAM"
    git clone --depth 1 https://github.com/WebAssembly/spec "$UPSTREAM"
else
    echo "Upstream repo present at $UPSTREAM (pulling latest shallow) ..."
    (cd "$UPSTREAM" && git fetch --depth 1 origin main 2>/dev/null || git fetch --depth 1) || true
fi

# 2. Locate wast2json (system package or python wabt package)
WAST2JSON="wast2json"
if ! command -v "$WAST2JSON" >/dev/null 2>&1; then
    # Try python-installed wabt package (used when system package missing)
    WAST2JSON="$(python3 -c '
import wabt, os, sys
w = wabt.Wabt()
name = "wast2json.exe" if sys.platform == "win32" else "wast2json"
path = os.path.join(w.bin_dir, name)
print(path if os.path.isfile(path) else "")
' 2>/dev/null || true)"
    if [ -z "$WAST2JSON" ] || [ ! -f "$WAST2JSON" ]; then
        echo "ERROR: wast2json not found (install wabt or python wabt package)" >&2
        exit 1
    fi
    echo "Using wast2json from python package: $WAST2JSON"
fi

echo "wast2json version: $($WAST2JSON --version | head -n 1)"

mkdir -p "$OUTDIR"

# 3. Convert MVP-relevant .wast files (full core except post-MVP feature tests)
# We run all of test/core/*.wast; filtered out post-MVP feature suites happen
# via the script's classification, not here.
count=0
for wast in "$UPSTREAM/test/core"/*.wast; do
    [ -e "$wast" ] || continue
    basename="$(basename "$wast" .wast)"
    out_json="$OUTDIR/$basename.json"
    # wast2json writes $basename.json + $basename.N.wasm beside it.
    # To keep outputs contained in OUTDIR we run from OUTDIR, so the
    # input path must be absolute (a relative one would break after
    # the change of directory).
    wast_abs="$(cd "$(dirname "$wast")" && pwd)/$(basename "$wast")"
    (cd "$OUTDIR" && "$WAST2JSON" --enable-all "$wast_abs" -o "$basename.json") || {
        echo "WARNING: conversion failed for $basename" >&2
        continue
    }
    count=$((count+1))
done

if [ "$count" -eq 0 ]; then
    echo "ERROR: no .wast file was converted (wast2json: $WAST2JSON)" >&2
    exit 1
fi

echo "Converted $count .wast files into $OUTDIR/"
echo "Binaries (.wasm) are gitignored; script committed so CI can reproduce."
