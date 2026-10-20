#!/bin/sh
# tools/check_no_alloc.sh — hard invariant 1 (zero dynamic allocation).
#
# Every engine, test and fuzz source must be free of dynamic allocation.
# The check requires the allocation API name to be followed by '(' so
# prose mentions in comments ("no malloc") do not count as violations.
#
# Exits 0 when the tree is clean, 1 otherwise.

set -u

DIRS="src include tests fuzz tools targets"
# The allocation APIs as *calls*: the name must be preceded by a
# character that cannot be part of an identifier (so "dependency-free("
# in a comment does not count) and be followed by an opening parenthesis.
PAT='(^|[^A-Za-z0-9_.-])(malloc|calloc|realloc|alloca|strdup|free)[[:space:]]*\('

hits=$(grep -rnE '(^|[^A-Za-z0-9_.-])(malloc|calloc|realloc|alloca|strdup|free)[[:space:]]*\(' \
        $DIRS --include='*.c' --include='*.h' 2>/dev/null)
if [ -n "$hits" ]; then
    echo "FAIL: dynamic allocation call(s) found:"
    echo "$hits"
    exit 1
fi
echo "OK: no dynamic allocation calls in $DIRS"
exit 0
