/*
 * test_phase14.c — real clang output and the typed-local regression.
 *
 * Two groups of checks:
 *
 * 1. The real-world corpus: the committed clang output in realworld/
 *    (qsort, f32 matrix multiply, CRC-32) is parsed, validated,
 *    instantiated and executed by the engine, and the results are
 *    compared with the values the native build produces (recorded in
 *    realworld/expected.txt). This is the readiness proof that the
 *    engine runs real compiler output.
 *
 * 2. The regression for the bug the real-world corpus exposed: an
 *    uninitialized local must be created with its DECLARED type (an
 *    f32 local starts at +0.0f), not with an i32 zero. A module that
 *    reads an f32 local before writing it traps with a type mismatch
 *    when the default is wrong.
 *
 * Standalone, C11, assert-style. Zero allocation: every buffer is a
 * static array.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wasm_host.h"
#include "wasm_memory.h"
#include "wasm_parser.h"
#include "wasm_validate.h"
#include "wasm_vm.h"
#include "harness.h"


static wasm_vm_t g_vm;
static wasm_host_registry_t g_reg;
static wasm_memory_t g_mem;
static uint8_t g_arena[65536];
static uint8_t g_file[65536];

/* Run the exported function "main" of a module image and return its
 * result. Returns 0 when the module could not be run at all. */
static int run_main(const uint8_t *image, size_t len, wasm_val_t *out)
{
    wasm_module_t mod;
    wasm_slice_t pay;
    uint32_t fidx = 0u;
    const wasm_func_desc_t *d;
    wasm_frame_t *top = NULL;
    wasm_trap_t tr;

    if (wasm_parse_header(image, len, &mod) != WASM_OK) {
        return 0;
    }
    if (wasm_validate_module(&mod) != WASM_OK) {
        return 0;
    }
    if (wasm_vm_init(&g_vm) != WASM_TRAP_OK) {
        return 0;
    }
    if (wasm_host_registry_init(&g_reg) != WASM_TRAP_OK) {
        return 0;
    }
    if (wasm_memory_init(&g_mem, g_arena, (uint32_t)sizeof(g_arena), 1u, 1u) !=
        WASM_TRAP_OK) {
        return 0;
    }
    if (wasm_vm_bind_memory(&g_vm, &g_mem) != WASM_TRAP_OK) {
        return 0;
    }
    if (wasm_instantiate(&g_vm, &mod, &g_reg, NULL) != WASM_OK) {
        return 0;
    }
    {
        wasm_section_header_t sec;
        if (wasm_find_section(&mod, (uint8_t)WASM_SECT_EXPORT, &sec) != WASM_OK) {
            return 0;
        }
        pay = sec.payload;
    }
    if (wasm_get_export_func_index(&pay, "main", &fidx) != WASM_OK) {
        return 0;
    }
    d = &g_vm.funcs[fidx];
    if (wasm_vm_enter_function(&g_vm, fidx, 0u, d->local_count,
                               d->return_count) != WASM_TRAP_OK) {
        return 0;
    }
    if (wasm_vm_attach_code(&g_vm, d->code, d->code_len) != WASM_TRAP_OK) {
        return 0;
    }
    for (;;) {
        if (wasm_call_stack_top(&g_vm.calls, &top) != WASM_TRAP_OK) {
            return 0;
        }
        if (top->code_ptr == NULL || top->code_end == NULL ||
            top->code_ptr >= top->code_end) {
            tr = wasm_vm_exit_function(&g_vm, top->return_count);
            break;
        }
        tr = wasm_vm_step(&g_vm);
        if (tr != WASM_TRAP_OK) {
            return 0; /* trap: the module is broken */
        }
    }
    if (tr != WASM_TRAP_OK) {
        return 0;
    }
    if (wasm_stack_pop(&g_vm.operands, out) != WASM_TRAP_OK) {
        return 0;
    }
    return 1;
}

static int load_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    size_t n;
    if (fp == NULL) {
        return 0;
    }
    n = fread(g_file, 1u, sizeof(g_file), fp);
    fclose(fp);
    *out_len = n;
    return n > 0u;
}

/* ---- 1. the real-world corpus ---- */

static void test_realworld_qsort(void)
{
    size_t len = 0u;
    wasm_val_t v;
    printf("[T] real clang output: qsort of a fixed int array\n");
    if (!load_file("realworld/qsort_demo.wasm", &len)) {
        CHECK(0, "realworld/qsort_demo.wasm is present");
        return;
    }
    CHECK(run_main(g_file, len, &v), "qsort_demo runs");
    CHECK(v.type == WASM_TYPE_I32, "qsort_demo returns i32");
    CHECK(v.value.i32 == 3286, "qsort_demo result == native (3286)");
}

static void test_realworld_matmul(void)
{
    size_t len = 0u;
    wasm_val_t v;
    printf("[T] real clang output: 8x8 f32 matrix multiply\n");
    if (!load_file("realworld/matmul_f32.wasm", &len)) {
        CHECK(0, "realworld/matmul_f32.wasm is present");
        return;
    }
    CHECK(run_main(g_file, len, &v), "matmul_f32 runs");
    CHECK(v.type == WASM_TYPE_F32, "matmul_f32 returns f32");
    CHECK(v.value.f32 == 22.125f, "matmul_f32 result == native (22.125)");
}

static void test_realworld_crc32(void)
{
    size_t len = 0u;
    wasm_val_t v;
    printf("[T] real clang output: CRC-32 over a fixed buffer\n");
    if (!load_file("realworld/crc32_demo.wasm", &len)) {
        CHECK(0, "realworld/crc32_demo.wasm is present");
        return;
    }
    CHECK(run_main(g_file, len, &v), "crc32_demo runs");
    CHECK(v.type == WASM_TYPE_I32, "crc32_demo returns i32");
    CHECK(v.value.i32 == 1157425541, "crc32_demo result == native (1157425541)");
}

/* ---- 2. regression: typed local defaults ---- */

/*
 * (func (result f32) (local f32) (f32.abs (local.get 0)))
 * The local is read before it is written: the engine must create it as
 * an f32 (+0.0f), otherwise the f32.abs traps with a type mismatch.
 */
static const uint8_t kF32LocalModule[] = {
    0x00u, 0x61u, 0x73u, 0x6Du, 0x01u, 0x00u, 0x00u, 0x00u,
    0x01u, 0x05u, 0x01u, 0x60u, 0x00u, 0x01u, 0x7Du, /* type: () -> f32 */
    0x03u, 0x02u, 0x01u, 0x00u,                         /* func 0 : type 0 */
    0x07u, 0x08u, 0x01u, 0x04u, 0x6Du, 0x61u, 0x69u, 0x6Eu,
    0x00u, 0x00u,                                       /* export "main" */
    0x0Au, 0x09u, 0x01u, 0x07u, 0x01u, 0x01u, 0x7Du, 0x20u,
    0x00u, 0x8Bu, 0x0Bu                                 /* locals: 1 x f32; local.get 0; f32.abs; end */
};

static void test_typed_local_default(void)
{
    wasm_val_t v;
    printf("[T] regression: an uninitialized local keeps its declared type\n");
    CHECK(run_main(kF32LocalModule, sizeof(kF32LocalModule), &v),
          "module with an f32 local runs");
    CHECK(v.type == WASM_TYPE_F32, "the result is f32");
    CHECK(v.value.f32 == 0.0f, "the uninitialized f32 local is +0.0f");
}

int main(void)
{
    static const harness_test_t tests[] = {
        { "realworld_qsort", test_realworld_qsort },
        { "realworld_matmul", test_realworld_matmul },
        { "realworld_crc32", test_realworld_crc32 },
        { "typed_local_default", test_typed_local_default },
    };
    printf("=== Phase 14 tests: real-world modules and typed locals ===\n");
    return harness_run("phase14", tests,
                       sizeof(tests) / sizeof(tests[0]));
}
