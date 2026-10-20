/*
 * afl_entry.c — AFL++ persistent-mode entry point for the shared pipeline.
 *
 * Built only when AFL++ (afl-clang-fast) is detected. The AFL runtime
 * owns the test-case buffer; the engine pipeline stays zero-allocation.
 */

#include <stddef.h>
#include <stdint.h>

#include "fuzz_pipeline.h"

__AFL_FUZZ_INIT();

int main(void)
{
    __AFL_INIT();
    while (__AFL_LOOP(100000)) {
        int len = __AFL_FUZZ_TESTCASE_LEN;
        (void)wasm_fuzz_pipeline(__AFL_FUZZ_TESTCASE_BUF, (size_t)len);
    }
    return 0;
}
