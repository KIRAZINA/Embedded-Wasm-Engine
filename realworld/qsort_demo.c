/*
 * realworld/qsort_demo.c — readiness program 1: integer sort.
 *
 * Real clang output, not a hand-assembled module: the array is sorted
 * with an insertion sort and folded into a checksum. Exercises integer
 * ALU, nested control flow, short-circuit evaluation and memory
 * loads/stores.
 *
 * The input and the work array are `volatile` on purpose: it keeps the
 * compiler from folding the whole computation into a constant, so the
 * module really contains the sort loop that the engine has to execute.
 *
 * The wasm build exports the entry point under the name "main" (the
 * runner resolves exports by name). The native build (-DNATIVE_BUILD)
 * prints the same line the runner prints for a wasm result, so both
 * outputs can be compared byte for byte.
 */

#if defined(NATIVE_BUILD)
#include <stdio.h>
#endif

#if defined(__wasm__)
#define EXPORT_MAIN __attribute__((export_name("main")))
#else
#define EXPORT_MAIN
#endif

#define ARRAY_LEN 16

static volatile int kInput[ARRAY_LEN] = {
    5, -3, 17, 0, 42, -8, 3, 99, -1, 7, 12, -20, 6, 31, -5, 14
};

static volatile int kWork[ARRAY_LEN];

static int compute(void)
{
    int i;
    int j;
    int key;
    int sum = 0;

    for (i = 0; i < ARRAY_LEN; i++) {
        kWork[i] = kInput[i];
    }
    /* insertion sort */
    for (i = 1; i < ARRAY_LEN; i++) {
        key = kWork[i];
        j = i - 1;
        while (j >= 0 && kWork[j] > key) {
            kWork[j + 1] = kWork[j];
            j = j - 1;
        }
        kWork[j + 1] = key;
    }
    /* weighted checksum of the sorted array */
    for (i = 0; i < ARRAY_LEN; i++) {
        sum = sum + (i + 1) * kWork[i];
    }
    return sum;
}

#if defined(NATIVE_BUILD)
int main(void)
{
    printf("result: %d (i32)\n", compute());
    return 0;
}
#else
EXPORT_MAIN int wasm_entry(void)
{
    return compute();
}
#endif
