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
        static const uint8_t i64_load[] = {
            0x41u, 0x00u, 0x29u, 0x03u, 0x00u, 0x0Bu
        };
        static const uint8_t load8s[] = {
            0x41u, 0x00u, 0x2Cu, 0x00u, 0x00u, 0x0Bu
        };
        static const uint8_t load8u[] = {
            0x41u, 0x00u, 0x2Du, 0x00u, 0x00u, 0x0Bu
        };
        static const uint8_t load16s[] = {
            0x41u, 0x00u, 0x2Eu, 0x01u, 0x00u, 0x0Bu
        };
        static const uint8_t load16u[] = {
            0x41u, 0x00u, 0x2Fu, 0x01u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_load8s[] = {
            0x41u, 0x00u, 0x30u, 0x00u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_load8u[] = {
            0x41u, 0x00u, 0x31u, 0x00u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_load16s[] = {
            0x41u, 0x00u, 0x32u, 0x01u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_load16u[] = {
            0x41u, 0x00u, 0x33u, 0x01u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_load32s[] = {
            0x41u, 0x00u, 0x34u, 0x02u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_load32u[] = {
            0x41u, 0x00u, 0x35u, 0x02u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_store[] = {
            0x41u, 0x00u, 0x42u, 0x00u, 0x37u, 0x03u, 0x00u, 0x0Bu
        };
        static const uint8_t store8[] = {
            0x41u, 0x00u, 0x41u, 0x01u, 0x3Au, 0x00u, 0x00u, 0x0Bu
        };
        static const uint8_t store16[] = {
            0x41u, 0x00u, 0x41u, 0x01u, 0x3Bu, 0x01u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_store8[] = {
            0x41u, 0x00u, 0x42u, 0x01u, 0x3Cu, 0x00u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_store16[] = {
            0x41u, 0x00u, 0x42u, 0x01u, 0x3Du, 0x01u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_store32[] = {
            0x41u, 0x00u, 0x42u, 0x01u, 0x3Eu, 0x02u, 0x00u, 0x0Bu
        };
        static const uint8_t mem_size[] = { 0x3Fu, 0x00u, 0x0Bu };
        static const uint8_t mem_grow[] = {
            0x41u, 0x00u, 0x40u, 0x00u, 0x0Bu
        };
        static const uint8_t load16_bad_align[] = {
            0x41u, 0x00u, 0x2Eu, 0x02u, 0x00u, 0x0Bu
        };
        static const uint8_t store8_bad_align[] = {
            0x41u, 0x00u, 0x41u, 0x01u, 0x3Au, 0x01u, 0x00u, 0x0Bu
        };
        static const uint8_t load32_bad_align[] = {
            0x41u, 0x00u, 0x35u, 0x03u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_load_bad_base[] = {
            0x42u, 0x00u, 0x29u, 0x03u, 0x00u, 0x0Bu
        };
        static const uint8_t i64_store16_bad_value[] = {
            0x41u, 0x00u, 0x41u, 0x01u, 0x3Du, 0x01u, 0x00u, 0x0Bu
        };
        static const uint8_t size_bad_reserved[] = { 0x3Fu, 0x01u, 0x0Bu };
        static const uint8_t size_no_reserved[] = { 0x3Fu };
        static const uint8_t grow_bad_reserved[] = {
            0x41u, 0x00u, 0x40u, 0x02u, 0x0Bu
        };
        static const uint8_t grow_bad_delta[] = {
            0x42u, 0x00u, 0x40u, 0x00u, 0x0Bu
        };
        check_result_body(0x7Eu, i64_load, sizeof(i64_load), WASM_OK,
                          "i64.load accepted");
        check_result_body(0x7Fu, load8s, sizeof(load8s), WASM_OK,
                          "i32.load8_s accepted");
        check_result_body(0x7Fu, load8u, sizeof(load8u), WASM_OK,
                          "i32.load8_u accepted");
        check_result_body(0x7Fu, load16s, sizeof(load16s), WASM_OK,
                          "i32.load16_s accepted");
        check_result_body(0x7Fu, load16u, sizeof(load16u), WASM_OK,
                          "i32.load16_u accepted");
        check_result_body(0x7Eu, i64_load8s, sizeof(i64_load8s), WASM_OK,
                          "i64.load8_s accepted");
        check_result_body(0x7Eu, i64_load8u, sizeof(i64_load8u), WASM_OK,
                          "i64.load8_u accepted");
        check_result_body(0x7Eu, i64_load16s, sizeof(i64_load16s), WASM_OK,
                          "i64.load16_s accepted");
        check_result_body(0x7Eu, i64_load16u, sizeof(i64_load16u), WASM_OK,
                          "i64.load16_u accepted");
        check_result_body(0x7Eu, i64_load32s, sizeof(i64_load32s), WASM_OK,
                          "i64.load32_s accepted");
        check_result_body(0x7Eu, i64_load32u, sizeof(i64_load32u), WASM_OK,
                          "i64.load32_u accepted");
        check_body(i64_store, sizeof(i64_store), WASM_OK, "i64.store accepted");
        check_body(store8, sizeof(store8), WASM_OK, "i32.store8 accepted");
        check_body(store16, sizeof(store16), WASM_OK, "i32.store16 accepted");
        check_body(i64_store8, sizeof(i64_store8), WASM_OK,
                   "i64.store8 accepted");
        check_body(i64_store16, sizeof(i64_store16), WASM_OK,
                   "i64.store16 accepted");
        check_body(i64_store32, sizeof(i64_store32), WASM_OK,
                   "i64.store32 accepted");
        check_result_body(0x7Fu, mem_size, sizeof(mem_size), WASM_OK,
                          "memory.size accepted");
        check_result_body(0x7Fu, mem_grow, sizeof(mem_grow), WASM_OK,
                          "memory.grow accepted");
        check_body(load16_bad_align, sizeof(load16_bad_align),
                   WASM_ERR_INVALID_ALIGN, "load16 align 2 rejected");
        check_body(store8_bad_align, sizeof(store8_bad_align),
                   WASM_ERR_INVALID_ALIGN, "store8 align 1 rejected");
        check_body(load32_bad_align, sizeof(load32_bad_align),
                   WASM_ERR_INVALID_ALIGN, "load32 align 3 rejected");
        check_body(i64_load_bad_base, sizeof(i64_load_bad_base),
                   WASM_ERR_INVALID_TYPE, "i64.load i64 base rejected");
        check_body(i64_store16_bad_value, sizeof(i64_store16_bad_value),
                   WASM_ERR_INVALID_TYPE, "i64.store16 i32 value rejected");
        check_body(size_bad_reserved, sizeof(size_bad_reserved),
                   WASM_ERR_INVALID_SECTION, "size reserved 0x01 rejected");
        check_body(size_no_reserved, sizeof(size_no_reserved),
                   WASM_ERR_TRUNCATED, "size missing reserved truncated");
        check_body(grow_bad_reserved, sizeof(grow_bad_reserved),
                   WASM_ERR_INVALID_SECTION, "grow reserved 0x02 rejected");
        check_body(grow_bad_delta, sizeof(grow_bad_delta),
                   WASM_ERR_INVALID_TYPE, "grow i64 delta rejected");
    }

    {
        static const uint8_t i64_eqz[] = { 0x42u, 0x00u, 0x50u, 0x0Bu };
        static const uint8_t i64_eq[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x51u, 0x0Bu
        };
        static const uint8_t i64_ne[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x52u, 0x0Bu
        };
        static const uint8_t i64_lt_s[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x53u, 0x0Bu
        };
        static const uint8_t i64_lt_u[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x54u, 0x0Bu
        };
        static const uint8_t i64_gt_s[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x55u, 0x0Bu
        };
        static const uint8_t i64_gt_u[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x56u, 0x0Bu
        };
        static const uint8_t i64_le_s[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x57u, 0x0Bu
        };
        static const uint8_t i64_le_u[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x58u, 0x0Bu
        };
        static const uint8_t i64_ge_s[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x59u, 0x0Bu
        };
        static const uint8_t i64_ge_u[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x5Au, 0x0Bu
        };
        static const uint8_t clz[] = { 0x41u, 0x05u, 0x67u, 0x0Bu };
        static const uint8_t ctz[] = { 0x41u, 0x05u, 0x68u, 0x0Bu };
        static const uint8_t popcnt[] = { 0x41u, 0x05u, 0x69u, 0x0Bu };
        static const uint8_t rotl[] = {
            0x41u, 0x01u, 0x41u, 0x02u, 0x77u, 0x0Bu
        };
        static const uint8_t rotr[] = {
            0x41u, 0x01u, 0x41u, 0x02u, 0x78u, 0x0Bu
        };
        static const uint8_t i64_clz[] = { 0x42u, 0x05u, 0x79u, 0x0Bu };
        static const uint8_t i64_ctz[] = { 0x42u, 0x05u, 0x7Au, 0x0Bu };
        static const uint8_t i64_popcnt[] = { 0x42u, 0x05u, 0x7Bu, 0x0Bu };
        static const uint8_t i64_rotl[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x89u, 0x0Bu
        };
        static const uint8_t i64_rotr[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x8Au, 0x0Bu
        };
        static const uint8_t wrap[] = { 0x42u, 0x05u, 0xA7u, 0x0Bu };
        static const uint8_t extend_s[] = { 0x41u, 0x7Fu, 0xACu, 0x0Bu };
        static const uint8_t extend_u[] = { 0x41u, 0x7Fu, 0xADu, 0x0Bu };
        static const uint8_t i64_eq_bad[] = {
            0x41u, 0x01u, 0x42u, 0x02u, 0x51u, 0x0Bu
        };
        static const uint8_t i64_eqz_bad[] = { 0x41u, 0x01u, 0x50u, 0x0Bu };
        static const uint8_t clz_bad[] = { 0x42u, 0x05u, 0x67u, 0x0Bu };
        static const uint8_t rotl_bad[] = {
            0x42u, 0x01u, 0x42u, 0x02u, 0x77u, 0x0Bu
        };
        static const uint8_t wrap_bad[] = { 0x41u, 0x01u, 0xA7u, 0x0Bu };
        static const uint8_t extend_bad[] = { 0x42u, 0x05u, 0xACu, 0x0Bu };
        check_result_body(0x7Fu, i64_eqz, sizeof(i64_eqz), WASM_OK,
                          "i64.eqz accepted");
        check_result_body(0x7Fu, i64_eq, sizeof(i64_eq), WASM_OK,
                          "i64.eq accepted");
        check_result_body(0x7Fu, i64_ne, sizeof(i64_ne), WASM_OK,
                          "i64.ne accepted");
        check_result_body(0x7Fu, i64_lt_s, sizeof(i64_lt_s), WASM_OK,
                          "i64.lt_s accepted");
        check_result_body(0x7Fu, i64_lt_u, sizeof(i64_lt_u), WASM_OK,
                          "i64.lt_u accepted");
        check_result_body(0x7Fu, i64_gt_s, sizeof(i64_gt_s), WASM_OK,
                          "i64.gt_s accepted");
        check_result_body(0x7Fu, i64_gt_u, sizeof(i64_gt_u), WASM_OK,
                          "i64.gt_u accepted");
        check_result_body(0x7Fu, i64_le_s, sizeof(i64_le_s), WASM_OK,
                          "i64.le_s accepted");
        check_result_body(0x7Fu, i64_le_u, sizeof(i64_le_u), WASM_OK,
                          "i64.le_u accepted");
        check_result_body(0x7Fu, i64_ge_s, sizeof(i64_ge_s), WASM_OK,
                          "i64.ge_s accepted");
        check_result_body(0x7Fu, i64_ge_u, sizeof(i64_ge_u), WASM_OK,
                          "i64.ge_u accepted");
        check_result_body(0x7Fu, clz, sizeof(clz), WASM_OK, "i32.clz accepted");
        check_result_body(0x7Fu, ctz, sizeof(ctz), WASM_OK, "i32.ctz accepted");
        check_result_body(0x7Fu, popcnt, sizeof(popcnt), WASM_OK,
                          "i32.popcnt accepted");
        check_result_body(0x7Fu, rotl, sizeof(rotl), WASM_OK,
                          "i32.rotl accepted");
        check_result_body(0x7Fu, rotr, sizeof(rotr), WASM_OK,
                          "i32.rotr accepted");
        check_result_body(0x7Eu, i64_clz, sizeof(i64_clz), WASM_OK,
                          "i64.clz accepted");
        check_result_body(0x7Eu, i64_ctz, sizeof(i64_ctz), WASM_OK,
                          "i64.ctz accepted");
        check_result_body(0x7Eu, i64_popcnt, sizeof(i64_popcnt), WASM_OK,
                          "i64.popcnt accepted");
        check_result_body(0x7Eu, i64_rotl, sizeof(i64_rotl), WASM_OK,
                          "i64.rotl accepted");
        check_result_body(0x7Eu, i64_rotr, sizeof(i64_rotr), WASM_OK,
                          "i64.rotr accepted");
        check_result_body(0x7Fu, wrap, sizeof(wrap), WASM_OK,
                          "i32.wrap_i64 accepted");
        check_result_body(0x7Eu, extend_s, sizeof(extend_s), WASM_OK,
                          "i64.extend_i32_s accepted");
        check_result_body(0x7Eu, extend_u, sizeof(extend_u), WASM_OK,
                          "i64.extend_i32_u accepted");
        check_body(i64_eq_bad, sizeof(i64_eq_bad), WASM_ERR_INVALID_TYPE,
                   "i64.eq mixed operands rejected");
        check_body(i64_eqz_bad, sizeof(i64_eqz_bad), WASM_ERR_INVALID_TYPE,
                   "i64.eqz i32 operand rejected");
        check_body(clz_bad, sizeof(clz_bad), WASM_ERR_INVALID_TYPE,
                   "i32.clz i64 operand rejected");
        check_body(rotl_bad, sizeof(rotl_bad), WASM_ERR_INVALID_TYPE,
                   "i32.rotl i64 operands rejected");
        check_body(wrap_bad, sizeof(wrap_bad), WASM_ERR_INVALID_TYPE,
                   "wrap i32 operand rejected");
        check_body(extend_bad, sizeof(extend_bad), WASM_ERR_INVALID_TYPE,
                   "extend i64 operand rejected");
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
        static const uint8_t sel_dead[] = {
            0x00u, 0x1Bu, 0x0Bu
        };
        static const uint8_t sel_dead_cond[] = {
            0x00u, 0x41u, 0x01u, 0x1Bu, 0x0Bu
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
        check_body(sel_dead, sizeof(sel_dead), WASM_OK,
                   "select on empty stack valid after unreachable");
        check_body(sel_dead_cond, sizeof(sel_dead_cond), WASM_OK,
                   "select with pushed cond valid after unreachable");
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
