/*
 * libfuzzer_entry.c — libFuzzer entry point for the shared pipeline.
 *
 * Built only with `clang -fsanitize=fuzzer`. The fuzzer runtime owns the
 * corpus and the input buffers; the engine pipeline itself stays
 * zero-allocation (static buffers in fuzz_pipeline.c).
 */

#include <stddef.h>
#include <stdint.h>

#include "fuzz_pipeline.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    (void)wasm_fuzz_pipeline(data, size);
    return 0;
}
