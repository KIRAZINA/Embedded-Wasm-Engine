/*
 * realworld/matmul_f32.c — readiness program 2: 8x8 f32 matrix multiply.
 *
 * Real clang output. Multiplies two 8x8 f32 matrices stored in the data
 * section and returns the sum of the result. Exercises the f32 ALU
 * (mul/add), f32 loads and stores, nested loops and array indexing.
 *
 * Every input value is a multiple of 0.25 and small, so every product
 * and every partial sum is exactly representable in binary32: the
 * native and the wasm result are bit-identical, independent of the
 * evaluation order or of fused multiply-add.
 *
 * The wasm build exports the entry point under the name "main" and
 * returns the f32 result directly, so the runner renders it exactly
 * like the native build prints it.
 */

#if defined(NATIVE_BUILD)
#include <stdio.h>
#endif

#if defined(__wasm__)
#define EXPORT_MAIN __attribute__((export_name("main")))
#else
#define EXPORT_MAIN
#endif

#include "matmul_data.h"

#define DIM 8
#define LEN (DIM * DIM)

static volatile float kA[LEN] = MATMUL_A;
static volatile float kB[LEN] = MATMUL_B;
static volatile float kC[LEN];

static float compute(void)
{
    int i;
    int j;
    int k;
    float acc;
    float total = 0.0f;

    for (i = 0; i < DIM; i++) {
        for (j = 0; j < DIM; j++) {
            acc = 0.0f;
            for (k = 0; k < DIM; k++) {
                acc = acc + kA[i * DIM + k] * kB[k * DIM + j];
            }
            kC[i * DIM + j] = acc;
        }
    }
    for (i = 0; i < LEN; i++) {
        total = total + kC[i];
    }
    return total;
}

#if defined(NATIVE_BUILD)
int main(void)
{
    printf("result: %.9g (f32)\n", (double)compute());
    return 0;
}
#else
EXPORT_MAIN float wasm_entry(void)
{
    return compute();
}
#endif
