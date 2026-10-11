#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_host.h"
#include "wasm_memory.h"
#include "wasm_parser.h"
#include "wasm_validate.h"
#include "wasm_vm.h"

#ifndef FUZZ_MAX_BYTES
#define FUZZ_MAX_BYTES 256u /* random input buffer bytes */
#endif
#ifndef FUZZ_PAGE_COUNT
#define FUZZ_PAGE_COUNT 1u /* pages supplied to each fuzzed VM */
#endif
#ifndef FUZZ_STEP_BUDGET
#define FUZZ_STEP_BUDGET 10u /* execution steps per fuzz case */
#endif

static uint64_t next_random(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13u; x ^= x >> 7u; x ^= x << 17u;
    *state = x;
    return x;
}

static uint32_t parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (s == NULL || out == NULL || *s == '\0') return 0u;
    v = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || v > 0xFFFFFFFFul) return 0u;
    *out = (uint32_t)v;
    return 1u;
}

static const uint8_t k_valid_prefix[] = {
    0x00u, 0x61u, 0x73u, 0x6Du, 0x01u, 0x00u, 0x00u, 0x00u,
    0x01u, 0x04u, 0x01u, 0x60u, 0x00u, 0x00u,
    0x03u, 0x02u, 0x01u, 0x00u,
    0x07u, 0x08u, 0x01u, 0x04u, 0x6Du, 0x61u, 0x69u, 0x6Eu,
    0x00u, 0x00u, 0x0Au, 0x05u, 0x01u, 0x03u, 0x00u, 0x41u, 0x00u
};

int main(int argc, char **argv)
{
    uint64_t state;
    uint32_t iterations, i, parsed = 0u, instantiated = 0u, executed = 0u;
    static uint8_t bytes[FUZZ_MAX_BYTES];
    static uint8_t arena[FUZZ_PAGE_COUNT * WASM_PAGE_SIZE];
    if (argc != 3 || !parse_u32(argv[2], &iterations)) {
        fprintf(stderr, "usage: %s SEED ITERATIONS\n", argv[0]);
        return 2;
    }
    state = strtoull(argv[1], NULL, 0);
    if (state == 0u) state = 0x9E3779B97F4A7C15ULL;
    for (i = 0u; i < iterations; i++) {
        size_t len = 8u + (size_t)(next_random(&state) % (FUZZ_MAX_BYTES - 8u));
        wasm_module_t mod;
        wasm_vm_t vm;
        wasm_memory_t mem;
        wasm_host_registry_t reg;
        wasm_err_t ec;
        wasm_trap_t tr;
        uint32_t fidx = 0u;
        uint32_t budget = FUZZ_STEP_BUDGET;
        memcpy(bytes, k_valid_prefix, sizeof(k_valid_prefix));
        if ((next_random(&state) & 1u) != 0u) {
            len = sizeof(k_valid_prefix);
            for (size_t j = 8u; j < len; j++) {
                if ((next_random(&state) & 7u) == 0u) bytes[j] ^= (uint8_t)next_random(&state);
            }
        } else {
            for (size_t j = 0u; j < len; j++) bytes[j] = (uint8_t)next_random(&state);
        }
        ec = wasm_parse_header(bytes, len, &mod);
        if (ec != WASM_OK) continue;
        parsed++;
        if (wasm_vm_init(&vm) != WASM_TRAP_OK ||
            wasm_host_registry_init(&reg) != WASM_TRAP_OK ||
            wasm_memory_init(&mem, arena, (uint32_t)sizeof(arena), 0u,
                             FUZZ_PAGE_COUNT) != WASM_TRAP_OK ||
            wasm_vm_bind_memory(&vm, &mem) != WASM_TRAP_OK) continue;
        ec = wasm_instantiate(&vm, &mod, &reg, NULL);
        if (ec != WASM_OK) continue;
        instantiated++;
        ec = wasm_find_section(&mod, (uint8_t)WASM_SECT_EXPORT,
                               &(wasm_section_header_t){0});
        if (ec != WASM_OK) continue;
        {
            wasm_section_header_t ex;
            wasm_slice_t payload;
            if (wasm_find_section(&mod, (uint8_t)WASM_SECT_EXPORT, &ex) != WASM_OK) continue;
            payload = ex.payload;
            if (wasm_get_export_func_index(&payload, "main", &fidx) != WASM_OK ||
                fidx >= WASM_MAX_FUNCTIONS || !vm.funcs[fidx].valid ||
                vm.funcs[fidx].param_count != 0u) continue;
        }
        if (wasm_vm_enter_function(&vm, fidx, 0u, vm.funcs[fidx].local_count,
                                   vm.funcs[fidx].return_count) != WASM_TRAP_OK ||
            wasm_vm_attach_code(&vm, vm.funcs[fidx].code,
                                vm.funcs[fidx].code_len) != WASM_TRAP_OK) continue;
        executed++;
        while (wasm_call_stack_depth(&vm.calls) > 0u && budget-- > 0u) {
            tr = wasm_vm_step(&vm);
            if (tr != WASM_TRAP_OK) break;
        }
    }
    {
        uint64_t report_seed = strtoull(argv[1], NULL, 0);
        printf("FUZZ seed=0x%08x%08x iterations=%u parsed=%u instantiated=%u executed=%u\n",
           (unsigned)(report_seed >> 32u), (unsigned)report_seed,
           (unsigned)iterations, (unsigned)parsed, (unsigned)instantiated,
           (unsigned)executed);
    }
    return 0;
}
