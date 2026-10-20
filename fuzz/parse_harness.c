/*
 * parse_harness.c — file/stdin entry point for the fuzzing pipeline.
 *
 *   parse_harness <module.wasm>   # one candidate from a file
 *   parse_harness < module.wasm   # one candidate from stdin
 *
 * Reads at most FUZZ_MAX_MODULE_BYTES into a static buffer (no malloc)
 * and runs the shared pipeline: parse -> section walk -> validate ->
 * instantiate -> bounded execution.
 *
 * Exit status is always 0 for a *handled* input: a crash (signal) is
 * the fuzzing signal, not a non-zero exit.
 */

#include <stdio.h>

#include "fuzz_pipeline.h"

static uint8_t g_input[FUZZ_MAX_MODULE_BYTES];

int main(int argc, char **argv)
{
    size_t n = 0u;
    int stages;

    if (argc > 1) {
        FILE *fp = fopen(argv[1], "rb");
        if (fp == NULL) {
            fprintf(stderr, "parse_harness: cannot open %s\n", argv[1]);
            return 2;
        }
        n = fread(g_input, 1u, sizeof(g_input), fp);
        fclose(fp);
    } else {
        n = fread(g_input, 1u, sizeof(g_input), stdin);
    }

    stages = wasm_fuzz_pipeline(g_input, n);
    printf("PIPELINE bytes=%u stages=%d\n", (unsigned)n, stages);
    return 0;
}
