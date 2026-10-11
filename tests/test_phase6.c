/*
 * test_phase6.c — Phase 6 integration: host callbacks, export lookup,
 * import binding, whole-module instantiation, arg/return round-trips.
 * Standalone, C11, stdio.h/stdint.h only. Hand-assembled MVP modules.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Werror -Iinclude <SRC> src/wasm_host.c
 *        tests/test_phase6.c -o build/test_phase6
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_host.h"
#include "wasm_parser.h"
#include "wasm_vm.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  [PASS] %s\n", (name)); g_pass++; } \
    else { printf("  [FAIL] %s (line %d)\n", (name), __LINE__); g_fail++; } \
} while (0)

static wasm_vm_t g_vm;
static wasm_host_registry_t g_reg;

/* M1: main(x:i32)->i32 { x + 41 }. No imports. */
static const uint8_t kM1[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7F, 0x01, 0x7F,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x00,
    0x0A, 0x08, 0x01, 0x06, 0x00, 0x20, 0x00, 0x41, 0x29, 0x6A
};

/* M2: import env.print_i32(i32)->(); main()->i32 { print(7); 35 }. */
static const uint8_t kM2[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x09, 0x02, 0x60, 0x01, 0x7F, 0x00, 0x60, 0x00, 0x01, 0x7F,
    0x02, 0x11, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x09,
    0x70, 0x72, 0x69, 0x6E, 0x74, 0x5F, 0x69, 0x33, 0x32, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x01,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x01,
    0x0A, 0x09, 0x01, 0x07, 0x00, 0x41, 0x07, 0x10, 0x00, 0x41, 0x23
};

/* M3: import env.print_i32 declared (i32,i32)->() — mismatches a (1,0) host. */
static const uint8_t kM3[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0A, 0x02, 0x60, 0x02, 0x7F, 0x7F, 0x00, 0x60, 0x00, 0x01, 0x7F,
    0x02, 0x11, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x09,
    0x70, 0x72, 0x69, 0x6E, 0x74, 0x5F, 0x69, 0x33, 0x32, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x01,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x01,
    0x0A, 0x09, 0x01, 0x07, 0x00, 0x41, 0x07, 0x10, 0x00, 0x41, 0x23
};

/* M4: import env.nope(i32)->() — no such host function. */
static const uint8_t kM4[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x09, 0x02, 0x60, 0x01, 0x7F, 0x00, 0x60, 0x00, 0x01, 0x7F,
    0x02, 0x0C, 0x01, 0x03, 0x65, 0x6E, 0x76, 0x04,
    0x6E, 0x6F, 0x70, 0x65, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x01,
    0x07, 0x08, 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x00, 0x01,
    0x0A, 0x09, 0x01, 0x07, 0x00, 0x41, 0x07, 0x10, 0x00, 0x41, 0x23
};

/* M5: header + memory(min 2). M6: header + memory(flags 2 -> malformed). */
static const uint8_t kM5[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x02
};
static const uint8_t kM6[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x05, 0x03, 0x01, 0x02, 0x02
};

static int32_t g_printed = 0;
static uint32_t g_print_calls = 0u;

static wasm_trap_t test_print(struct wasm_vm *vm,
                              const wasm_val_t *args, uint32_t nargs,
                              wasm_val_t *rets, uint32_t nrets)
{
    (void)vm;
    (void)rets;
    (void)nrets;
    if (args == NULL || nargs < 1u) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    if (args[0].type != WASM_TYPE_I32) {
        return WASM_TRAP_TYPE_MISMATCH;
    }
    g_printed = args[0].value.i32;
    g_print_calls++;
    return WASM_TRAP_OK;
}

static wasm_trap_t test_add(struct wasm_vm *vm,
                            const wasm_val_t *args, uint32_t nargs,
                            wasm_val_t *rets, uint32_t nrets)
{
    (void)vm;
    if (args == NULL || rets == NULL || nargs < 2u || nrets < 1u) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    if (args[0].type != WASM_TYPE_I32 || args[1].type != WASM_TYPE_I32) {
        return WASM_TRAP_TYPE_MISMATCH;
    }
    rets[0].type = WASM_TYPE_I32;
    rets[0].value.i32 = args[0].value.i32 + args[1].value.i32;
    return WASM_TRAP_OK;
}

static wasm_val_t mk_i32(int32_t v)
{
    wasm_val_t x;
    x.type = WASM_TYPE_I32;
    x.value.i32 = v;
    return x;
}

/* Fresh VM, empty frame, attach, run to completion. */
static wasm_trap_t run_flat(const uint8_t *code, size_t len)
{
    wasm_trap_t tr = wasm_vm_init(&g_vm);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_vm_attach_code(&g_vm, code, len);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    return wasm_vm_run(&g_vm);
}

static void expect_i64(int64_t want, const char *name)
{
    wasm_val_t v;
    wasm_trap_t tr = wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I64, &v);
    CHECK(tr == WASM_TRAP_OK && v.value.i64 == want, name);
}

/* Parse bytes -> module (fails the CHECK on bad fixture). */
static int parse_mod(const uint8_t *b, size_t n, wasm_module_t *m, const char *n_)
{
    wasm_err_t ec = wasm_parse_header(b, n, m);
    CHECK(ec == WASM_OK, n_);
    return ec == WASM_OK ? 1 : 0;
}

static wasm_slice_t payload_of(const wasm_module_t *m, uint8_t id, int *ok)
{
    wasm_section_header_t h;
    wasm_slice_t empty;
    empty.data = NULL;
    empty.len = 0u;
    if (wasm_find_section(m, id, &h) != WASM_OK) {
        *ok = 0;
        return empty;
    }
    {
        wasm_slice_t s;
        s.data = h.payload.data;
        s.len = h.payload.len;
        *ok = 1;
        return s;
    }
}

/* ---------- a) host callbacks invoked from WASM bytecode via OP_CALL ---------- */

static void test_host_call(void)
{
    static const uint8_t add_code[] = { 0x41, 0x06, 0x41, 0x07, 0x10, 0x03 };
    static const uint8_t print_code[] = { 0x41, 0x2A, 0x10, 0x04 };
    wasm_val_t v;
    printf("[T] host callbacks via OP_CALL from bytecode\n");
    g_printed = 0;
    g_print_calls = 0u;
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm init");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_vm_define_host_function(&g_vm, 3u, 2u, 1u, test_add) == WASM_TRAP_OK, "define host add(2,1)");
    CHECK(wasm_vm_define_host_function(&g_vm, 4u, 1u, 0u, test_print) == WASM_TRAP_OK, "define host print(1,0)");
    CHECK(wasm_vm_enter_function(&g_vm, 0u, 0u, 0u, 0u) == WASM_TRAP_OK, "enter");
    CHECK(wasm_vm_attach_code(&g_vm, add_code, sizeof(add_code)) == WASM_TRAP_OK, "attach add call");
    CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run 6+7 via host");
    CHECK(wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I32, &v) == WASM_TRAP_OK && v.value.i32 == 13, "host add returned 13");
    CHECK(wasm_vm_attach_code(&g_vm, print_code, sizeof(print_code)) == WASM_TRAP_OK, "attach print call");
    CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run print(42)");
    CHECK(g_print_calls == 1u && g_printed == 42, "callback observed 42 once");
    CHECK(wasm_stack_is_empty(&g_vm.operands) == 1, "void host left nothing behind");
    CHECK(wasm_vm_exit_function(&g_vm, 0u) == WASM_TRAP_OK, "exit");
    /* registrar validation. */
    CHECK(wasm_vm_define_host_function(&g_vm, 32u, 0u, 0u, test_print) == WASM_TRAP_INVALID_ARGUMENT, "host idx 32 rejected");
    CHECK(wasm_vm_define_host_function(&g_vm, 0u, 33u, 0u, test_print) == WASM_TRAP_INVALID_ARGUMENT, "host 33 params rejected");
    CHECK(wasm_vm_define_host_function(&g_vm, 0u, 0u, 2u, test_print) == WASM_TRAP_INVALID_ARGUMENT, "host 2 results rejected");
    CHECK(wasm_vm_define_host_function(&g_vm, 0u, 0u, 0u, NULL) == WASM_TRAP_NULL, "host NULL cb rejected");
    CHECK(wasm_vm_define_host_function(NULL, 0u, 0u, 0u, test_print) == WASM_TRAP_NULL, "host NULL vm rejected");
    CHECK(wasm_host_register_func(&g_reg, "env", "print_i32", 1u, 0u, test_print) == WASM_TRAP_OK, "registry add ok");
    CHECK(wasm_host_register_func(&g_reg, "env", "toolongname0123456789abcdefghijk", 0u, 0u, test_print) == WASM_TRAP_INVALID_ARGUMENT, "registry overlong name rejected");
    CHECK(wasm_host_register_func(&g_reg, "env", "x", 0u, 0u, NULL) == WASM_TRAP_NULL, "registry NULL cb rejected");
    CHECK(wasm_host_register_func(NULL, "env", "x", 0u, 0u, test_print) == WASM_TRAP_NULL, "registry NULL reg rejected");
}

/* ---------- b) export table lookup ---------- */

static void test_export_lookup(void)
{
    wasm_module_t m;
    wasm_slice_t pay;
    int ok = 0;
    uint32_t idx = 0xFFFFFFFFu;
    printf("[T] export table parsing + name lookup\n");
    if (!parse_mod(kM1, sizeof(kM1), &m, "M1 parses")) {
        return;
    }
    pay = payload_of(&m, (uint8_t)WASM_SECT_EXPORT, &ok);
    CHECK(ok == 1, "export section present");
    CHECK(wasm_get_export_func_index(&pay, "main", &idx) == WASM_OK && idx == 0u, "lookup main -> 0");
    CHECK(wasm_get_export_func_index(&pay, "nope", &idx) == WASM_ERR_EXPORT_NOT_FOUND, "lookup nope -> NOT_FOUND");
    CHECK(wasm_get_export_func_index(&pay, "mai", &idx) == WASM_ERR_EXPORT_NOT_FOUND, "prefix mai -> NOT_FOUND");
    CHECK(wasm_get_export_func_index(&pay, "mainx", &idx) == WASM_ERR_EXPORT_NOT_FOUND, "mainx -> NOT_FOUND");
    CHECK(wasm_get_export_func_index(&pay, "", &idx) == WASM_ERR_EXPORT_NOT_FOUND, "empty name -> NOT_FOUND");
    CHECK(wasm_get_export_func_index(NULL, "main", &idx) == WASM_ERR_NULL, "NULL payload rejected");
    CHECK(wasm_get_export_func_index(&pay, NULL, &idx) == WASM_ERR_NULL, "NULL name rejected");
    CHECK(wasm_get_export_func_index(&pay, "main", NULL) == WASM_ERR_NULL, "NULL out rejected");
    { /* empty payload: structurally fine, nothing inside. */
        wasm_slice_t e;
        e.data = NULL;
        e.len = 0u;
        CHECK(wasm_get_export_func_index(&e, "main", &idx) == WASM_ERR_EXPORT_NOT_FOUND, "empty payload -> NOT_FOUND");
    }
    { /* truncated name bytes. */
        const uint8_t b[] = { 0x01, 0x04, 0x6D, 0x61 };
        wasm_slice_t s;
        s.data = b;
        s.len = sizeof(b);
        CHECK(wasm_get_export_func_index(&s, "main", &idx) == WASM_ERR_TRUNCATED, "cut name -> TRUNCATED");
    }
    { /* cut ULEB count. */
        const uint8_t b[] = { 0x80 };
        wasm_slice_t s;
        s.data = b;
        s.len = sizeof(b);
        CHECK(wasm_get_export_func_index(&s, "main", &idx) == WASM_ERR_LEB_TRUNCATED, "cut count -> LEB_TRUNCATED");
    }
    { /* same name, non-function kind (memory): must not match. */
        const uint8_t b[] = { 0x01, 0x04, 0x6D, 0x61, 0x69, 0x6E, 0x02, 0x00 };
        wasm_slice_t s;
        s.data = b;
        s.len = sizeof(b);
        CHECK(wasm_get_export_func_index(&s, "main", &idx) == WASM_ERR_EXPORT_NOT_FOUND, "memory-kind main ignored");
    }
}

/* ---------- c) args in, result out, end to end ---------- */

static void test_whole_module_call(void)
{
    wasm_module_t m;
    wasm_section_header_t ex;
    wasm_slice_t pay;
    wasm_val_t v;
    uint32_t idx = 0u, nfuncs = 0u;
    printf("[T] instantiate M1, call main(1) -> 42\n");
    if (!parse_mod(kM1, sizeof(kM1), &m, "M1 parses")) {
        return;
    }
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm init");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_instantiate(&g_vm, &m, &g_reg, &nfuncs) == WASM_OK && nfuncs == 1u, "instantiate, 1 func, no imports");
    CHECK(wasm_find_section(&m, (uint8_t)WASM_SECT_EXPORT, &ex) == WASM_OK, "export section found");
    pay.data = ex.payload.data;
    pay.len = ex.payload.len;
    CHECK(wasm_get_export_func_index(&pay, "main", &idx) == WASM_OK && idx == 0u, "main -> func 0");
    CHECK(wasm_stack_push(&g_vm.operands, mk_i32(1)) == WASM_TRAP_OK, "push arg 1");
    CHECK(wasm_vm_enter_function(&g_vm, idx, 1u, 0u, 1u) == WASM_TRAP_OK, "enter main(1p,0l,1r)");
    CHECK(wasm_vm_attach_code(&g_vm, g_vm.funcs[idx].code, g_vm.funcs[idx].code_len) == WASM_TRAP_OK, "attach body");
    CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "exit");
    CHECK(wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I32, &v) == WASM_TRAP_OK && v.value.i32 == 42, "1 + 41 == 42");
    CHECK(wasm_instantiate(NULL, &m, &g_reg, &nfuncs) == WASM_ERR_NULL, "instantiate NULL vm rejected");
    CHECK(wasm_instantiate(&g_vm, NULL, &g_reg, &nfuncs) == WASM_ERR_NULL, "instantiate NULL mod rejected");
}

/* ---------- d) missing imports, signature mismatch, bad exports ---------- */

static void test_link_errors(void)
{
    wasm_module_t m;
    wasm_slice_t ipp, tpp;
    int ok = 0;
    uint32_t map[WASM_MAX_FUNCTIONS];
    uint32_t n = 0u, idx = 0u;
    printf("[T] link errors without host crashes\n");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_func(&g_reg, "env", "print_i32", 1u, 0u, test_print) == WASM_TRAP_OK, "register print");
    /* M4 names an unknown host function. */
    if (!parse_mod(kM4, sizeof(kM4), &m, "M4 parses")) {
        return;
    }
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm init");
    CHECK(wasm_instantiate(&g_vm, &m, &g_reg, NULL) == WASM_ERR_UNRESOLVED_IMPORT, "env.nope -> UNRESOLVED_IMPORT");
    /* M3 declares (i32,i32)->() but the host offers (i32)->(). */
    if (!parse_mod(kM3, sizeof(kM3), &m, "M3 parses")) {
        return;
    }
    CHECK(wasm_instantiate(&g_vm, &m, &g_reg, NULL) == WASM_ERR_TYPE_MISMATCH_IMPORT, "2-param import vs 1-param host -> MISMATCH");
    /* Direct bind checks on M2's import section. */
    if (!parse_mod(kM2, sizeof(kM2), &m, "M2 parses")) {
        return;
    }
    ipp = payload_of(&m, (uint8_t)WASM_SECT_IMPORT, &ok);
    CHECK(ok == 1, "import section present");
    tpp = payload_of(&m, (uint8_t)WASM_SECT_TYPE, &ok);
    CHECK(ok == 1, "type section present");
    CHECK(wasm_bind_imports(&ipp, &tpp, &g_reg, map, (uint32_t)WASM_MAX_FUNCTIONS, &n) == WASM_OK && n == 1u && map[0] == 0u, "bind resolves to slot 0");
    CHECK(wasm_bind_imports(&ipp, NULL, &g_reg, map, (uint32_t)WASM_MAX_FUNCTIONS, &n) == WASM_OK && n == 1u, "bind without types skips check");
    CHECK(wasm_bind_imports(&ipp, &tpp, NULL, map, (uint32_t)WASM_MAX_FUNCTIONS, &n) == WASM_ERR_UNRESOLVED_IMPORT, "bind NULL registry -> UNRESOLVED");
    CHECK(wasm_bind_imports(&ipp, &tpp, &g_reg, map, 0u, &n) == WASM_ERR_SECTION_OVERFLOW, "bind cap 0 -> OVERFLOW");
    CHECK(wasm_bind_imports(NULL, &tpp, &g_reg, map, 16u, &n) == WASM_ERR_NULL, "bind NULL payload rejected");
    /* Export miss on a healthy module. */
    {
        wasm_slice_t pay = payload_of(&m, (uint8_t)WASM_SECT_EXPORT, &ok);
        CHECK(ok == 1, "M2 export section present");
        CHECK(wasm_get_export_func_index(&pay, "missing", &idx) == WASM_ERR_EXPORT_NOT_FOUND, "missing export -> NOT_FOUND");
    }
    /* Registry capacity: 16 slots, 17th fails, VM state untouched. */
    {
        wasm_host_registry_t full;
        uint32_t k;
        CHECK(wasm_host_registry_init(&full) == WASM_TRAP_OK, "full-reg init");
        for (k = 0u; k < (uint32_t)WASM_MAX_HOST_FUNCS; k++) {
            char nm[8];
            nm[0] = (char)('a' + (char)(k % 26u));
            nm[1] = '\0';
            if (wasm_host_register_func(&full, "m", nm, 0u, 0u, test_print) != WASM_TRAP_OK) {
                break;
            }
        }
        CHECK(k == (uint32_t)WASM_MAX_HOST_FUNCS, "16 slots fill");
        CHECK(wasm_host_register_func(&full, "m", "z", 0u, 0u, test_print) == WASM_TRAP_INVALID_ARGUMENT, "17th registration rejected");
    }
}

/* ---------- e) host import called from guest code (print(7) -> 35) ---------- */

static void test_guest_calls_host(void)
{
    wasm_module_t m;
    wasm_section_header_t ex;
    wasm_slice_t pay;
    wasm_val_t v;
    uint32_t idx = 0u, nfuncs = 0u;
    printf("[T] guest main calls imported env.print_i32\n");
    g_printed = 0;
    g_print_calls = 0u;
    if (!parse_mod(kM2, sizeof(kM2), &m, "M2 parses")) {
        return;
    }
    CHECK(wasm_vm_init(&g_vm) == WASM_TRAP_OK, "vm init");
    CHECK(wasm_host_registry_init(&g_reg) == WASM_TRAP_OK, "registry init");
    CHECK(wasm_host_register_func(&g_reg, "env", "print_i32", 1u, 0u, test_print) == WASM_TRAP_OK, "register print");
    CHECK(wasm_instantiate(&g_vm, &m, &g_reg, &nfuncs) == WASM_OK && nfuncs == 2u, "instantiate: 1 import + 1 defined");
    CHECK(g_vm.funcs[0].is_host == 1, "func 0 is the host import");
    CHECK(g_vm.funcs[1].is_host == 0, "func 1 is WASM code");
    CHECK(wasm_find_section(&m, (uint8_t)WASM_SECT_EXPORT, &ex) == WASM_OK, "export found");
    pay.data = ex.payload.data;
    pay.len = ex.payload.len;
    CHECK(wasm_get_export_func_index(&pay, "main", &idx) == WASM_OK && idx == 1u, "main -> func 1");
    CHECK(wasm_vm_enter_function(&g_vm, idx, 0u, 0u, 1u) == WASM_TRAP_OK, "enter main");
    CHECK(wasm_vm_attach_code(&g_vm, g_vm.funcs[idx].code, g_vm.funcs[idx].code_len) == WASM_TRAP_OK, "attach");
    CHECK(wasm_vm_run(&g_vm) == WASM_TRAP_OK, "run (host call inline, single run)");
    CHECK(g_print_calls == 1u && g_printed == 7, "host saw print(7)");
    CHECK(wasm_vm_exit_function(&g_vm, 1u) == WASM_TRAP_OK, "exit");
    CHECK(wasm_stack_pop_typed(&g_vm.operands, WASM_TYPE_I32, &v) == WASM_TRAP_OK && v.value.i32 == 35, "main returned 35");
}

/* ---------- f) memory declaration + static footprint ---------- */

static void test_memory_and_footprint(void)
{
    wasm_module_t m;
    uint32_t min = 0u;
    int present = -1;
    size_t engine;
    printf("[T] memory limits + <128KB engine footprint\n");
    if (!parse_mod(kM5, sizeof(kM5), &m, "M5 parses")) {
        return;
    }
    CHECK(wasm_get_memory_min(&m, &min, &present) == WASM_OK && present == 1 && min == 2u, "memory min 2 pages");
    if (!parse_mod(kM1, sizeof(kM1), &m, "M1 re-parses")) {
        return;
    }
    CHECK(wasm_get_memory_min(&m, &min, &present) == WASM_OK && present == 0 && min == 0u, "no memory section -> absent");
    if (!parse_mod(kM6, sizeof(kM6), &m, "M6 parses")) {
        return;
    }
    CHECK(wasm_get_memory_min(&m, &min, &present) == WASM_ERR_INVALID_SECTION, "flags=2 rejected");
    CHECK(wasm_get_memory_min(NULL, &min, &present) == WASM_ERR_NULL, "memory NULL mod rejected");
    engine = sizeof(wasm_vm_t) + sizeof(wasm_host_registry_t) +
             sizeof(wasm_memory_t) +
             (size_t)WASM_MAX_FUNCTIONS * sizeof(uint32_t);
    printf("  sizeof(vm)=%u sizeof(registry)=%u sizeof(mem)=%u engine=%u (limit %u)\n",
           (unsigned)sizeof(wasm_vm_t),
           (unsigned)sizeof(wasm_host_registry_t),
           (unsigned)sizeof(wasm_memory_t),
           (unsigned)engine, (unsigned)(128u * 1024u));
    CHECK(engine < (size_t)(128u * 1024u), "engine static footprint < 128KB");
}

/* ---------- g) i64 arithmetic core: ALU, traps, shifts ---------- */

static void test_i64_alu(void)
{
    /* SLEB64 note: values in [-64,63] encode in one byte; 2^32 is
     * 80 80 80 80 10; INT64_MIN is nine 0x80 bytes plus 0x7F. */
    static const uint8_t kAdd[] = { 0x42, 0x28, 0x42, 0x14, 0x7C };
    static const uint8_t kSub[] = { 0x42, 0x32, 0x42, 0x08, 0x7D };
    static const uint8_t kMul64[] = {
        0x42, 0x80, 0x80, 0x80, 0x80, 0x10, 0x42, 0x02, 0x7E
    };
    static const uint8_t kAddNeg[] = { 0x42, 0x7B, 0x42, 0x08, 0x7C };
    static const uint8_t kSubWrap[] = { 0x42, 0x00, 0x42, 0x01, 0x7D };
    static const uint8_t kDiv[] = { 0x42, 0x07, 0x42, 0x02, 0x7F };
    static const uint8_t kDivNeg[] = { 0x42, 0x79, 0x42, 0x02, 0x7F };
    static const uint8_t kDivU[] = { 0x42, 0x7F, 0x42, 0x02, 0x80 };
    static const uint8_t kRem[] = { 0x42, 0x07, 0x42, 0x02, 0x81 };
    static const uint8_t kRemNeg[] = { 0x42, 0x79, 0x42, 0x02, 0x81 };
    static const uint8_t kRemU[] = { 0x42, 0x7F, 0x42, 0x02, 0x82 };
    static const uint8_t kDiv0[] = { 0x42, 0x0A, 0x42, 0x00, 0x7F };
    static const uint8_t kDivU0[] = { 0x42, 0x0A, 0x42, 0x00, 0x80 };
    static const uint8_t kRem0[] = { 0x42, 0x0A, 0x42, 0x00, 0x81 };
    static const uint8_t kRemU0[] = { 0x42, 0x0A, 0x42, 0x00, 0x82 };
    /* i64.const INT64_MIN, i64.const -1, then the division op. */
    static const uint8_t kMinDiv[] = {
        0x42, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7F,
        0x42, 0x7F, 0x7F
    };
    static const uint8_t kMinRem[] = {
        0x42, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7F,
        0x42, 0x7F, 0x81
    };
    static const uint8_t kAnd[] = { 0x42, 0x0C, 0x42, 0x0A, 0x83 };
    static const uint8_t kOr[] = { 0x42, 0x0C, 0x42, 0x0A, 0x84 };
    static const uint8_t kXor[] = { 0x42, 0x0C, 0x42, 0x0A, 0x85 };
    static const uint8_t kShl63[] = { 0x42, 0x01, 0x42, 0x3F, 0x86 };
    static const uint8_t kShl64[] = { 0x42, 0x01, 0x42, 0xC0, 0x00, 0x86 };
    static const uint8_t kShrU[] = { 0x42, 0x7F, 0x42, 0x01, 0x88 };
    static const uint8_t kShrS[] = { 0x42, 0x7F, 0x42, 0x01, 0x87 };
    static const uint8_t kTypeMix[] = { 0x41, 0x01, 0x42, 0x02, 0x7C };
    static const uint8_t kBare[] = { 0x7C };

    printf("[T] i64 arithmetic, traps and shifts\n");
    CHECK(run_flat(kAdd, sizeof(kAdd)) == WASM_TRAP_OK, "run i64 40+20");
    expect_i64(60, "i64 40+20 == 60");
    CHECK(run_flat(kSub, sizeof(kSub)) == WASM_TRAP_OK, "run i64 50-8");
    expect_i64(42, "i64 50-8 == 42");
    CHECK(run_flat(kMul64, sizeof(kMul64)) == WASM_TRAP_OK, "run i64 2^32*2");
    expect_i64(INT64_C(8589934592), "i64 2^32*2 == 2^33 (64-bit range)");
    CHECK(run_flat(kAddNeg, sizeof(kAddNeg)) == WASM_TRAP_OK, "run i64 -5+8");
    expect_i64(3, "i64 -5+8 == 3");
    CHECK(run_flat(kSubWrap, sizeof(kSubWrap)) == WASM_TRAP_OK, "run i64 0-1");
    expect_i64(-1, "i64 0-1 wraps to -1 (no trap)");
    CHECK(run_flat(kDiv, sizeof(kDiv)) == WASM_TRAP_OK, "run i64 7/2");
    expect_i64(3, "i64 7/2 == 3");
    CHECK(run_flat(kDivNeg, sizeof(kDivNeg)) == WASM_TRAP_OK, "run i64 -7/2");
    expect_i64(-3, "i64 -7/2 == -3");
    CHECK(run_flat(kDivU, sizeof(kDivU)) == WASM_TRAP_OK, "run i64 u64max/2");
    expect_i64(INT64_MAX, "i64 0xFFFF..FFFF/2 == INT64_MAX");
    CHECK(run_flat(kRem, sizeof(kRem)) == WASM_TRAP_OK, "run i64 7%2");
    expect_i64(1, "i64 7%2 == 1");
    CHECK(run_flat(kRemNeg, sizeof(kRemNeg)) == WASM_TRAP_OK, "run i64 -7%2");
    expect_i64(-1, "i64 -7%2 == -1");
    CHECK(run_flat(kRemU, sizeof(kRemU)) == WASM_TRAP_OK, "run i64 u64max%2");
    expect_i64(1, "i64 0xFFFF..FFFF%2 == 1");
    CHECK(run_flat(kDiv0, sizeof(kDiv0)) == WASM_TRAP_DIV_BY_ZERO, "i64 10/0 -> DIV_BY_ZERO");
    CHECK(run_flat(kDivU0, sizeof(kDivU0)) == WASM_TRAP_DIV_BY_ZERO, "i64 10/0u -> DIV_BY_ZERO");
    CHECK(run_flat(kRem0, sizeof(kRem0)) == WASM_TRAP_DIV_BY_ZERO, "i64 10%0 -> DIV_BY_ZERO");
    CHECK(run_flat(kRemU0, sizeof(kRemU0)) == WASM_TRAP_DIV_BY_ZERO, "i64 10%0u -> DIV_BY_ZERO");
    CHECK(run_flat(kMinDiv, sizeof(kMinDiv)) == WASM_TRAP_INTEGER_OVERFLOW, "i64 MIN/-1 div -> INTEGER_OVERFLOW");
    CHECK(run_flat(kMinRem, sizeof(kMinRem)) == WASM_TRAP_INTEGER_OVERFLOW, "i64 MIN%-1 rem -> INTEGER_OVERFLOW");
    CHECK(run_flat(kAnd, sizeof(kAnd)) == WASM_TRAP_OK, "run i64 12&10");
    expect_i64(8, "i64 12&10 == 8");
    CHECK(run_flat(kOr, sizeof(kOr)) == WASM_TRAP_OK, "run i64 12|10");
    expect_i64(14, "i64 12|10 == 14");
    CHECK(run_flat(kXor, sizeof(kXor)) == WASM_TRAP_OK, "run i64 12^10");
    expect_i64(6, "i64 12^10 == 6");
    CHECK(run_flat(kShl63, sizeof(kShl63)) == WASM_TRAP_OK, "run i64 1<<63");
    expect_i64(INT64_MIN, "i64 1<<63 == INT64_MIN");
    CHECK(run_flat(kShl64, sizeof(kShl64)) == WASM_TRAP_OK, "run i64 1<<64");
    expect_i64(1, "i64 shift count masked %64 (1<<64 == 1)");
    CHECK(run_flat(kShrU, sizeof(kShrU)) == WASM_TRAP_OK, "run i64 -1>>1 logical");
    expect_i64(INT64_MAX, "i64 -1>>u1 == INT64_MAX");
    CHECK(run_flat(kShrS, sizeof(kShrS)) == WASM_TRAP_OK, "run i64 -1>>1 arith");
    expect_i64(-1, "i64 -1>>s1 == -1");
    CHECK(run_flat(kTypeMix, sizeof(kTypeMix)) == WASM_TRAP_TYPE_MISMATCH, "i32+i64 add -> TYPE_MISMATCH");
    CHECK(run_flat(kBare, sizeof(kBare)) == WASM_TRAP_STACK_UNDERFLOW, "bare i64.add -> UNDERFLOW");
}

/* ---------- h) CLI --dump output scraping ---------- */

static int dump_contains(const char *haystack, const char *needle, const char *name)
{
    int found = (strstr(haystack, needle) != NULL) ? 1 : 0;
    CHECK(found == 1, name);
    return found;
}

static void test_cli_dump(void)
{
#ifdef _WIN32
    static const char *const cmd_add =
        "build\\wasm_runner.exe --dump demo\\add.wasm > build\\dump_add.txt";
    static const char *const cmd_print =
        "build\\wasm_runner.exe --dump demo\\print.wasm > build\\dump_print.txt";
#else
    static const char *const cmd_add =
        "./build/wasm_runner --dump demo/add.wasm > build/dump_add.txt";
    static const char *const cmd_print =
        "./build/wasm_runner --dump demo/print.wasm > build/dump_print.txt";
#endif
    static char out[4096];
    FILE *fp = NULL;
    size_t n = 0u;
    printf("[T] CLI --dump output\n");
    CHECK(system(cmd_add) == 0, "dump add.wasm exits 0");
    fp = fopen("build/dump_add.txt", "rb");
    CHECK(fp != NULL, "dump output captured");
    if (fp == NULL) {
        return;
    }
    n = fread(out, 1u, sizeof(out) - 1u, fp);
    fclose(fp);
    out[n] = '\0';
    dump_contains(out, "00 61 73 6d", "magic bytes shown");
    dump_contains(out, "version: 1", "version shown");
    dump_contains(out, "size: 41 bytes", "file size shown");
    dump_contains(out, "name=Type", "type section row");
    dump_contains(out, "name=Function", "function section row");
    dump_contains(out, "name=Export", "export section row");
    dump_contains(out, "name=Code", "code section row");
    dump_contains(out, "payload_off=10", "type payload offset");
    dump_contains(out, "functype 0", "type signature header");
    dump_contains(out, "params[i32, i32]", "add params decoded");
    dump_contains(out, "results[i32]", "add result decoded");
    dump_contains(out, "\"main\"", "export name shown");
    dump_contains(out, "kind=function", "export kind shown");
    dump_contains(out, "index=0", "export index shown");
    CHECK(strstr(out, "result:") == NULL, "dump executes nothing");
    CHECK(system(cmd_print) == 0, "dump print.wasm exits 0");
    fp = fopen("build/dump_print.txt", "rb");
    CHECK(fp != NULL, "print dump captured");
    if (fp == NULL) {
        return;
    }
    n = fread(out, 1u, sizeof(out) - 1u, fp);
    fclose(fp);
    out[n] = '\0';
    dump_contains(out, "name=Import", "import section row");
    dump_contains(out, "params[i32]", "print import params decoded");
    dump_contains(out, "results[]", "void result decoded");
    dump_contains(out, "params[]", "empty params decoded");
}

int main(void)
{
    printf("=== Phase 6 tests: host integration + import/export link ===\n");
    test_host_call();
    test_export_lookup();
    test_whole_module_call();
    test_link_errors();
    test_guest_calls_host();
    test_memory_and_footprint();
    test_i64_alu();
    test_cli_dump();
    printf("--------------------------------------------\n");
    printf("RESULT: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf("STATUS: FAIL\n");
        return 1;
    }
    printf("STATUS: ALL TESTS PASSED\n");
    return 0;
}
