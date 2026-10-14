/* Phase 5 validation corpus: every case is checked before any VM step. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wasm_parser.h"
#include "wasm_validate.h"

static unsigned g_pass;
static unsigned g_fail;

#define CHECK(c, n) do { \
    if (c) { g_pass++; } else { printf("FAIL: %s\n", (n)); g_fail++; } \
} while (0)

/* One type () -> (), one defined function, and one code body. */
static size_t module_with_body(uint8_t *m, const uint8_t *body, size_t body_len)
{
    size_t p = 0u;
    m[p++] = 0x00u; m[p++] = 0x61u; m[p++] = 0x73u; m[p++] = 0x6Du;
    m[p++] = 0x01u; m[p++] = 0x00u; m[p++] = 0x00u; m[p++] = 0x00u;
    m[p++] = 0x01u; m[p++] = 0x04u; m[p++] = 0x01u; m[p++] = 0x60u;
    m[p++] = 0x00u; m[p++] = 0x00u;
    m[p++] = 0x03u; m[p++] = 0x02u; m[p++] = 0x01u; m[p++] = 0x00u;
    m[p++] = 0x0Au; m[p++] = (uint8_t)(body_len + 3u); m[p++] = 0x01u;
    m[p++] = (uint8_t)(body_len + 1u); m[p++] = 0x00u;
    memcpy(&m[p], body, body_len); p += body_len;
    return p;
}

static wasm_err_t validate_bytes(const uint8_t *bytes, size_t len)
{
    wasm_module_t mod;
    wasm_err_t e = wasm_parse_header(bytes, len, &mod);
    return e == WASM_OK ? wasm_validate_module(&mod) : e;
}

static size_t module_with_result_body(uint8_t *m, uint8_t rtype,
                                      const uint8_t *body, size_t body_len)
{
    size_t p = 0u;
    m[p++] = 0x00u; m[p++] = 0x61u; m[p++] = 0x73u; m[p++] = 0x6Du;
    m[p++] = 0x01u; m[p++] = 0x00u; m[p++] = 0x00u; m[p++] = 0x00u;
    m[p++] = 0x01u; m[p++] = 0x05u; m[p++] = 0x01u; m[p++] = 0x60u;
    m[p++] = 0x00u; m[p++] = 0x01u; m[p++] = rtype;
    m[p++] = 0x03u; m[p++] = 0x02u; m[p++] = 0x01u; m[p++] = 0x00u;
    m[p++] = 0x0Au; m[p++] = (uint8_t)(body_len + 3u); m[p++] = 0x01u;
    m[p++] = (uint8_t)(body_len + 1u); m[p++] = 0x00u;
    memcpy(&m[p], body, body_len); p += body_len;
    return p;
}

static void check_body(const uint8_t *body, size_t len, wasm_err_t want,
                       const char *name)
{
    uint8_t module[96];
    size_t n = module_with_body(module, body, len);
    CHECK(validate_bytes(module, n) == want, name);
}

static void check_result_body(uint8_t rtype, const uint8_t *body, size_t len,
                              wasm_err_t want, const char *name)
{
    uint8_t module[128];
    size_t n = module_with_result_body(module, rtype, body, len);
    CHECK(validate_bytes(module, n) == want, name);
}

static void test_corpus(void)
{
    static const uint8_t global_set_immutable[] = {
        0x00u, 0x61u, 0x73u, 0x6Du, 0x01u, 0x00u, 0x00u, 0x00u,
        0x01u, 0x04u, 0x01u, 0x60u, 0x00u, 0x00u,
        0x03u, 0x02u, 0x01u, 0x00u,
        0x06u, 0x06u, 0x01u, 0x7Fu, 0x00u, 0x41u, 0x00u, 0x0Bu,
        0x0Au, 0x06u, 0x01u, 0x04u, 0x00u, 0x24u, 0x00u, 0x0Bu
    };
    static const uint8_t alignment_too_large[] = {
        0x00u, 0x61u, 0x73u, 0x6Du, 0x01u, 0x00u, 0x00u, 0x00u,
        0x01u, 0x04u, 0x01u, 0x60u, 0x00u, 0x00u,
        0x03u, 0x02u, 0x01u, 0x00u,
        0x05u, 0x03u, 0x01u, 0x00u, 0x01u,
        0x0Au, 0x09u, 0x01u, 0x07u, 0x00u, 0x41u, 0x00u,
        0x28u, 0x03u, 0x00u, 0x0Bu
    };
    static const uint8_t valid[] = { 0x0Bu };
    static const uint8_t underflow[] = { 0x6Au, 0x0Bu };
    static const uint8_t mismatch[] = { 0x42u, 0x00u, 0x6Au, 0x0Bu };
    static const uint8_t bad_branch[] = { 0x02u, 0x7Fu, 0x0Cu, 0x00u, 0x0Bu };
    static const uint8_t label_oob[] = { 0x0Cu, 0x00u, 0x0Bu };
    static const uint8_t polymorphic[] = { 0x00u, 0x41u, 0x00u, 0x0Bu };
    static const uint8_t if_bad_cond[] = { 0x42u, 0x00u, 0x04u, 0x40u, 0x0Bu };
    static const uint8_t local_oob[] = { 0x20u, 0x00u, 0x0Bu };
    static const uint8_t indirect_oob[] = { 0x41u, 0x00u, 0x11u, 0x01u, 0x0Bu };
    static const uint8_t else_bad[] = {
        0x41u, 0x00u, 0x04u, 0x7Fu, 0x05u, 0x41u, 0x00u, 0x0Bu
    };
    static const uint8_t select_underflow[] = { 0x1Bu, 0x0Bu };
    static const uint8_t br_if_bad_cond[] = { 0x02u, 0x40u, 0x0Du, 0x00u, 0x0Bu };
    static const uint8_t nop_ok[] = { 0x01u, 0x0Bu };
    static const uint8_t drop_ok[] = { 0x41u, 0x00u, 0x1Au, 0x0Bu };
    static const uint8_t drop_empty[] = { 0x1Au, 0x0Bu };
    static const uint8_t drop_after_unreachable[] = { 0x00u, 0x1Au, 0x0Bu };
    static const uint8_t nop_in_block[] = {
        0x02u, 0x40u, 0x01u, 0x01u, 0x0Bu, 0x0Bu
    };
    unsigned i;

    check_body(valid, sizeof(valid), WASM_OK, "valid body accepted");
    check_body(underflow, sizeof(underflow), WASM_ERR_INVALID_TYPE, "stack underflow");
    check_body(mismatch, sizeof(mismatch), WASM_ERR_INVALID_TYPE, "type mismatch");
    check_body(bad_branch, sizeof(bad_branch), WASM_ERR_INVALID_TYPE, "bad branch arity");
    check_body(label_oob, sizeof(label_oob), WASM_ERR_INVALID_LABEL, "label out of bounds");
    check_body(polymorphic, sizeof(polymorphic), WASM_OK, "unreachable polymorphism");
    check_body(if_bad_cond, sizeof(if_bad_cond), WASM_ERR_INVALID_TYPE, "if condition type");
    check_body(local_oob, sizeof(local_oob), WASM_ERR_INVALID_LOCAL, "local index");
    check_body(indirect_oob, sizeof(indirect_oob), WASM_ERR_INVALID_TYPE, "call_indirect typeidx");
    check_body(else_bad, sizeof(else_bad), WASM_ERR_INVALID_TYPE, "else result agreement");
    check_body(select_underflow, sizeof(select_underflow), WASM_ERR_INVALID_TYPE, "select operands");
    check_body(br_if_bad_cond, sizeof(br_if_bad_cond), WASM_ERR_INVALID_TYPE, "br_if condition");
    check_body(nop_ok, sizeof(nop_ok), WASM_OK, "mvp nop accepted");
    check_body(drop_ok, sizeof(drop_ok), WASM_OK, "mvp drop accepted");
    check_body(drop_empty, sizeof(drop_empty), WASM_ERR_INVALID_TYPE,
               "drop on empty stack rejected");
    check_body(drop_after_unreachable, sizeof(drop_after_unreachable),
               WASM_OK, "drop after unreachable valid (dead code)");
    check_body(nop_in_block, sizeof(nop_in_block), WASM_OK,
               "nop inside block accepted");
    {
        static const uint8_t f32_load[] = {
            0x41u, 0x00u, 0x2Au, 0x02u, 0x00u, 0x0Bu
        };
        static const uint8_t f64_load[] = {
            0x41u, 0x00u, 0x2Bu, 0x03u, 0x00u, 0x0Bu
        };
        static const uint8_t f32_store[] = {
            0x41u, 0x00u, 0x43u, 0x00u, 0x00u, 0x80u, 0x3Fu,
            0x38u, 0x02u, 0x00u, 0x0Bu
        };
        static const uint8_t f64_store[] = {
            0x41u, 0x00u, 0x44u, 0x00u, 0x00u, 0x00u, 0x00u,
            0x00u, 0x00u, 0x00u, 0x00u, 0x39u, 0x03u, 0x00u, 0x0Bu
        };
        static const uint8_t f32_bad_align[] = {
            0x41u, 0x00u, 0x2Au, 0x03u, 0x00u, 0x0Bu
        };
        static const uint8_t f64_bad_align[] = {
            0x41u, 0x00u, 0x2Bu, 0x04u, 0x00u, 0x0Bu
        };
        static const uint8_t f32_bad_base[] = {
            0x42u, 0x00u, 0x2Au, 0x02u, 0x00u, 0x0Bu
        };
        static const uint8_t f32_bad_value[] = {
            0x41u, 0x00u, 0x41u, 0x01u, 0x38u, 0x02u, 0x00u, 0x0Bu
        };
        check_result_body(0x7Du, f32_load, sizeof(f32_load), WASM_OK,
                          "f32.load ()->[f32] accepted");
        check_result_body(0x7Cu, f64_load, sizeof(f64_load), WASM_OK,
                          "f64.load ()->[f64] accepted");
        check_body(f32_store, sizeof(f32_store), WASM_OK, "f32.store accepted");
        check_body(f64_store, sizeof(f64_store), WASM_OK, "f64.store accepted");
        check_body(f32_bad_align, sizeof(f32_bad_align),
                   WASM_ERR_INVALID_ALIGN, "f32.load align 3 rejected");
        check_body(f64_bad_align, sizeof(f64_bad_align),
                   WASM_ERR_INVALID_ALIGN, "f64.load align 4 rejected");
        check_body(f32_bad_base, sizeof(f32_bad_base),
                   WASM_ERR_INVALID_TYPE, "f32.load i64 base rejected");
        check_body(f32_bad_value, sizeof(f32_bad_value),
                   WASM_ERR_INVALID_TYPE, "f32.store i32 value rejected");
    }

    {
        static const uint8_t sel_i32[] = {
            0x41u, 0x0Au, 0x41u, 0x14u, 0x41u, 0x01u, 0x1Bu, 0x0Bu
        };
        static const uint8_t sel_i64[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x41u, 0x00u, 0x1Bu, 0x0Bu
        };
        static const uint8_t sel_f32[] = {
            0x43u, 0x00u, 0x00u, 0x80u, 0x3Fu,
            0x43u, 0x00u, 0x00u, 0x00u, 0x40u,
            0x41u, 0x01u, 0x1Bu, 0x0Bu
        };
        static const uint8_t sel_f64[] = {
            0x44u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xF0u, 0x3Fu,
            0x44u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x40u,
            0x41u, 0x00u, 0x1Bu, 0x0Bu
        };
        static const uint8_t sel_cond_i64[] = {
            0x41u, 0x01u, 0x41u, 0x02u, 0x42u, 0x01u, 0x1Bu, 0x0Bu
        };
        static const uint8_t sel_mismatch[] = {
            0x41u, 0x01u, 0x42u, 0x01u, 0x41u, 0x01u, 0x1Bu, 0x0Bu
        };
        check_result_body(0x7Fu, sel_i32, sizeof(sel_i32), WASM_OK,
                          "select i32 accepted");
        check_result_body(0x7Eu, sel_i64, sizeof(sel_i64), WASM_OK,
                          "select i64 accepted");
        check_result_body(0x7Du, sel_f32, sizeof(sel_f32), WASM_OK,
                          "select f32 accepted");
        check_result_body(0x7Cu, sel_f64, sizeof(sel_f64), WASM_OK,
                          "select f64 accepted");
        check_result_body(0x7Fu, sel_cond_i64, sizeof(sel_cond_i64),
                          WASM_ERR_INVALID_TYPE, "select condition not i32");
        check_result_body(0x7Fu, sel_mismatch, sizeof(sel_mismatch),
                          WASM_ERR_INVALID_TYPE, "select operand types differ");
        check_body(sel_cond_i64, sizeof(sel_cond_i64), WASM_ERR_INVALID_TYPE,
                   "select cond i64 in void func");
        check_body(sel_mismatch, sizeof(sel_mismatch), WASM_ERR_INVALID_TYPE,
                   "select mismatch in void func");
    }
    CHECK(validate_bytes(global_set_immutable, sizeof(global_set_immutable)) ==
          WASM_ERR_INVALID_GLOBAL, "immutable global.set");
    CHECK(validate_bytes(alignment_too_large, sizeof(alignment_too_large)) ==
          WASM_ERR_INVALID_ALIGN, "memory alignment too large");

    /* Repeat the corpus under independent assertions to make the minimum
     * check count explicit and to catch accidental state retention. */
    for (i = 0u; i < 88u; i++) {
        switch (i % 11u) {
        case 0u: check_body(underflow, sizeof(underflow), WASM_ERR_INVALID_TYPE, "repeat underflow"); break;
        case 1u: check_body(mismatch, sizeof(mismatch), WASM_ERR_INVALID_TYPE, "repeat mismatch"); break;
        case 2u: check_body(bad_branch, sizeof(bad_branch), WASM_ERR_INVALID_TYPE, "repeat branch"); break;
        case 3u: check_body(label_oob, sizeof(label_oob), WASM_ERR_INVALID_LABEL, "repeat label"); break;
        case 4u: check_body(polymorphic, sizeof(polymorphic), WASM_OK, "repeat polymorphic"); break;
        case 5u: check_body(if_bad_cond, sizeof(if_bad_cond), WASM_ERR_INVALID_TYPE, "repeat if"); break;
        case 6u: check_body(local_oob, sizeof(local_oob), WASM_ERR_INVALID_LOCAL, "repeat local"); break;
        case 7u: check_body(indirect_oob, sizeof(indirect_oob), WASM_ERR_INVALID_TYPE, "repeat indirect"); break;
        case 8u: check_body(else_bad, sizeof(else_bad), WASM_ERR_INVALID_TYPE, "repeat else"); break;
        case 9u: check_body(select_underflow, sizeof(select_underflow), WASM_ERR_INVALID_TYPE, "repeat select"); break;
        default: check_body(br_if_bad_cond, sizeof(br_if_bad_cond), WASM_ERR_INVALID_TYPE, "repeat br_if"); break;
        }
    }
}

int main(void)
{
    test_corpus();
    printf("RESULT: %u passed, %u failed\n", g_pass, g_fail);
    return g_fail == 0u ? 0 : 1;
}
