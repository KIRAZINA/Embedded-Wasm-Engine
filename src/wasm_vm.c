/*
 * wasm_vm.c — function entry/exit + local.get/set/tee + Phase 4 step/run core.
 * C11, -Wall -Wextra -Werror clean. No malloc. No UB shifts/divides.
 */

#include "wasm_vm.h"

#include <math.h>
#include <string.h>

#include "leb128.h"
#include "wasm_control.h"

wasm_trap_t wasm_vm_init(wasm_vm_t *vm)
{
    wasm_trap_t tr;
    uint32_t i;
    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    tr = wasm_stack_init(&vm->operands);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_call_stack_init(&vm->calls);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    vm->mem = NULL;
    for (i = 0u; i < (uint32_t)WASM_MAX_FUNCTIONS; i++) {
        uint32_t k;
        vm->funcs[i].valid = 0;
        vm->funcs[i].is_host = 0;
        vm->funcs[i].host_cb = NULL;
        for (k = 0u; k < (uint32_t)WASM_MAX_LOCALS_PER_FRAME; k++) {
            vm->funcs[i].param_types[k] = 0u;
        }
        vm->funcs[i].return_type = 0u;
        vm->funcs[i].has_sig = 0;
    }
    for (i = 0u; i < (uint32_t)WASM_MAX_GLOBALS; i++) {
        vm->globals[i].value.type = WASM_TYPE_I32;
        vm->globals[i].value.value.i32 = 0;
        vm->globals[i].mut = 0;
        vm->globals[i].valid = 0;
    }
    vm->nglobals = 0u;
    for (i = 0u; i < (uint32_t)WASM_MAX_TABLE_SIZE; i++) {
        vm->table[i] = (uint32_t)WASM_TABLE_EMPTY;
    }
    vm->table_size = 0u;
    vm->typesec = NULL;
    vm->typesec_len = 0u;
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_vm_enter_function(wasm_vm_t *vm, uint32_t func_idx,
                                   uint32_t param_count, uint32_t local_count,
                                   uint32_t return_count)
{
    uint64_t total64;
    uint32_t total;
    uint32_t i;
    wasm_frame_t frame;
    wasm_val_t tmp[WASM_MAX_LOCALS_PER_FRAME];
    wasm_trap_t tr;

    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    if (return_count > 1u) {
        return WASM_TRAP_INVALID_ARGUMENT; /* MVP: at most one result */
    }
    total64 = (uint64_t)param_count + (uint64_t)local_count;
    if (total64 > (uint64_t)WASM_MAX_LOCALS_PER_FRAME) {
        return WASM_TRAP_INVALID_LOCAL_INDEX;
    }
    total = (uint32_t)total64;
    if (param_count > (uint32_t)WASM_MAX_LOCALS_PER_FRAME) {
        return WASM_TRAP_INVALID_LOCAL_INDEX;
    }
    /* Fail fast BEFORE mutating either stack. */
    if (wasm_call_stack_is_full(&vm->calls)) {
        return WASM_TRAP_CALL_STACK_OVERFLOW;
    }
    if (wasm_stack_depth(&vm->operands) < param_count) {
        return WASM_TRAP_INSUFFICIENT_ARGS;
    }

    /* Pop args last-first into scratch, then lay out P0..Pn in order. */
    for (i = 0u; i < param_count; i++) {
        tr = wasm_stack_pop(&vm->operands, &tmp[param_count - 1u - i]);
        if (tr != WASM_TRAP_OK) {
            return tr; /* unreachable: depth pre-checked, kept for safety */
        }
    }
    frame.func_idx = func_idx;
    frame.stack_base_ptr = wasm_stack_depth(&vm->operands);
    frame.locals_count = total;
    frame.return_count = return_count;
    frame.code_ptr = NULL;
    frame.code_end = NULL;
    (void)wasm_control_stack_init(&frame.ctrl); /* fresh frame: no open blocks */
    for (i = 0u; i < param_count; i++) {
        frame.locals[i] = tmp[i];
    }
    /* Zero-init declared locals: deterministic I32 zero. */
    for (i = param_count; i < total; i++) {
        frame.locals[i].type = WASM_TYPE_I32;
        frame.locals[i].value.i32 = 0;
    }

    return wasm_call_stack_push(&vm->calls, &frame);
}

wasm_trap_t wasm_vm_exit_function(wasm_vm_t *vm, uint32_t return_count)
{
    wasm_frame_t *top = NULL;
    wasm_trap_t tr;
    wasm_val_t ret;
    int have_ret = 0;

    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    if (return_count > 1u) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    tr = wasm_call_stack_top(&vm->calls, &top);
    if (tr != WASM_TRAP_OK) {
        return tr; /* empty -> CALL_STACK_UNDERFLOW */
    }
    if (return_count != top->return_count) {
        return WASM_TRAP_INVALID_ARGUMENT; /* arity must match entry */
    }

    /* Validate everything before mutating: return value must exist ... */
    if (return_count == 1u) {
        tr = wasm_stack_pop(&vm->operands, &ret);
        if (tr != WASM_TRAP_OK) {
            return tr; /* missing return -> STACK_UNDERFLOW, state kept */
        }
        have_ret = 1;
    }
    /* ... and the frame base must be sane (callee must not eat caller data). */
    if (top->stack_base_ptr > wasm_stack_depth(&vm->operands)) {
        /* Restore the popped return value before reporting. */
        if (have_ret) {
            (void)wasm_stack_push(&vm->operands, ret);
        }
        return WASM_TRAP_STACK_UNDERFLOW;
    }
    /* ... and re-pushing the result must fit (post-truncation top == base). */
    if (have_ret && top->stack_base_ptr >= (uint32_t)WASM_STACK_MAX_DEPTH) {
        (void)wasm_stack_push(&vm->operands, ret);
        return WASM_TRAP_STACK_OVERFLOW;
    }

    /* Commit: truncate garbage, re-push the result, drop the frame. */
    vm->operands.top = top->stack_base_ptr;
    if (have_ret) {
        tr = wasm_stack_push(&vm->operands, ret);
        if (tr != WASM_TRAP_OK) {
            return tr; /* unreachable: capacity pre-checked above */
        }
    }
    return wasm_call_stack_pop(&vm->calls, NULL);
}

static wasm_trap_t vm_top_frame(wasm_vm_t *vm, wasm_frame_t **out)
{
    wasm_trap_t tr;
    if (vm == NULL || out == NULL) {
        return WASM_TRAP_NULL;
    }
    tr = wasm_call_stack_top(&vm->calls, out);
    return tr;
}

wasm_trap_t wasm_vm_local_get(wasm_vm_t *vm, uint32_t local_idx)
{
    wasm_frame_t *f = NULL;
    wasm_trap_t tr;
    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    tr = vm_top_frame(vm, &f);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    if (local_idx >= f->locals_count) {
        return WASM_TRAP_INVALID_LOCAL_INDEX;
    }
    return wasm_stack_push(&vm->operands, f->locals[local_idx]);
}

wasm_trap_t wasm_vm_local_set(wasm_vm_t *vm, uint32_t local_idx)
{
    wasm_frame_t *f = NULL;
    wasm_trap_t tr;
    wasm_val_t v;
    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    tr = vm_top_frame(vm, &f);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    if (local_idx >= f->locals_count) {
        return WASM_TRAP_INVALID_LOCAL_INDEX;
    }
    tr = wasm_stack_pop(&vm->operands, &v);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    f->locals[local_idx] = v;
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_vm_local_tee(wasm_vm_t *vm, uint32_t local_idx)
{
    wasm_frame_t *f = NULL;
    wasm_trap_t tr;
    wasm_val_t v;
    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    tr = vm_top_frame(vm, &f);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    if (local_idx >= f->locals_count) {
        return WASM_TRAP_INVALID_LOCAL_INDEX;
    }
    tr = wasm_stack_peek(&vm->operands, &v);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    f->locals[local_idx] = v;
    return WASM_TRAP_OK;
}

/* ================= Phase 4: execution core ================= */

/* Reinterpret u32 bits as i32 without aliasing UB. */
static int32_t vm_u2s(uint32_t u)
{
    int32_t r;
    memcpy(&r, &u, sizeof(r));
    return r;
}

static wasm_trap_t vm_push_i32(wasm_vm_t *vm, int32_t v)
{
    wasm_val_t x;
    x.type = WASM_TYPE_I32;
    x.value.i32 = v;
    return wasm_stack_push(&vm->operands, x);
}

static wasm_trap_t vm_push_i64(wasm_vm_t *vm, int64_t v)
{
    wasm_val_t x;
    x.type = WASM_TYPE_I64;
    x.value.i64 = v;
    return wasm_stack_push(&vm->operands, x);
}

static wasm_trap_t vm_push_bool(wasm_vm_t *vm, int cond)
{
    return vm_push_i32(vm, cond ? 1 : 0);
}

/* Pop two I32 operands (b = top). Restores b if the second pop fails. */
static wasm_trap_t vm_pop2_i32(wasm_vm_t *vm, int32_t *a, int32_t *b)
{
    wasm_val_t va;
    wasm_val_t vb;
    wasm_trap_t tr;
    tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &va);
    if (tr != WASM_TRAP_OK) {
        (void)wasm_stack_push(&vm->operands, vb);
        return tr;
    }
    *a = va.value.i32;
    *b = vb.value.i32;
    return WASM_TRAP_OK;
}

/* Reinterpret u64 bits as i64 without aliasing UB. */
static int64_t vm_u2s64(uint64_t u)
{
    int64_t r;
    memcpy(&r, &u, sizeof(r));
    return r;
}

/* Pop two I64 operands (b = top). Restores b if the second pop fails. */
static wasm_trap_t vm_pop2_i64(wasm_vm_t *vm, int64_t *a, int64_t *b)
{
    wasm_val_t va;
    wasm_val_t vb;
    wasm_trap_t tr;
    tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I64, &vb);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I64, &va);
    if (tr != WASM_TRAP_OK) {
        (void)wasm_stack_push(&vm->operands, vb);
        return tr;
    }
    *a = va.value.i64;
    *b = vb.value.i64;
    return WASM_TRAP_OK;
}

/* Pop two F32 operands (b = top). Restores b if the second pop fails. */
static wasm_trap_t vm_pop2_f32(wasm_vm_t *vm, float *a, float *b)
{
    wasm_val_t va;
    wasm_val_t vb;
    wasm_trap_t tr;
    tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &vb);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &va);
    if (tr != WASM_TRAP_OK) {
        (void)wasm_stack_push(&vm->operands, vb);
        return tr;
    }
    *a = va.value.f32;
    *b = vb.value.f32;
    return WASM_TRAP_OK;
}

/* Pop two F64 operands (b = top). Restores b if the second pop fails. */
static wasm_trap_t vm_pop2_f64(wasm_vm_t *vm, double *a, double *b)
{
    wasm_val_t va;
    wasm_val_t vb;
    wasm_trap_t tr;
    tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &vb);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &va);
    if (tr != WASM_TRAP_OK) {
        (void)wasm_stack_push(&vm->operands, vb);
        return tr;
    }
    *a = va.value.f64;
    *b = vb.value.f64;
    return WASM_TRAP_OK;
}

static wasm_trap_t vm_push_f32(wasm_vm_t *vm, float v)
{
    wasm_val_t x;
    x.type = WASM_TYPE_F32;
    x.value.f32 = v;
    return wasm_stack_push(&vm->operands, x);
}

static wasm_trap_t vm_push_f64(wasm_vm_t *vm, double v)
{
    wasm_val_t x;
    x.type = WASM_TYPE_F64;
    x.value.f64 = v;
    return wasm_stack_push(&vm->operands, x);
}

/* ================= Float bit-pattern helpers (S6) =================
 * abs/neg/copysign/reinterpret never inspect float VALUE; they move bits
 * through unsigned integers via memcpy (no aliasing UB, no comparisons). */

static uint32_t vm_f32_bits(float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static float vm_bits_f32(uint32_t u)
{
    float v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

static uint64_t vm_f64_bits(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static double vm_bits_f64(uint64_t u)
{
    double v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

/* Canonical quiet NaNs emitted by min/max on NaN input (S2/S7).
 * Arithmetic (add/div/sqrt/...) propagates whatever NaN the host C
 * arithmetic produces; only min/max canonicalize, per spec latitude. */
#define VM_F32_CANON_NAN_BITS 0x7FC00000u
#define VM_F64_CANON_NAN_BITS 0x7FF8000000000000ULL

static float vm_f32_canon_nan(void)
{
    return vm_bits_f32((uint32_t)VM_F32_CANON_NAN_BITS);
}

static double vm_f64_canon_nan(void)
{
    return vm_bits_f64((uint64_t)VM_F64_CANON_NAN_BITS);
}

/* ================= WASM min/max (S2) =================
 * Hand-rolled: C fminf/fmaxf have the wrong NaN and signed-zero behavior.
 * NaN in either operand -> canonical quiet NaN; min(+0,-0) = -0 (and the
 * mirror); otherwise the C comparison result. */

static float vm_f32_min(float a, float b)
{
    uint32_t ua;
    uint32_t ub;
    if (isnan(a) || isnan(b)) {
        return vm_f32_canon_nan();
    }
    if (a == b) {
        ua = vm_f32_bits(a);
        ub = vm_f32_bits(b);
        if (ua == 0x80000000u || ub == 0x80000000u) {
            return vm_bits_f32(0x80000000u); /* min(+0,-0) = -0 */
        }
        return a;
    }
    return (a < b) ? a : b;
}

static float vm_f32_max(float a, float b)
{
    uint32_t ua;
    uint32_t ub;
    if (isnan(a) || isnan(b)) {
        return vm_f32_canon_nan();
    }
    if (a == b) {
        ua = vm_f32_bits(a);
        ub = vm_f32_bits(b);
        if (ua == 0x00000000u || ub == 0x00000000u) {
            return vm_bits_f32(0x00000000u); /* max(+0,-0) = +0 */
        }
        return a;
    }
    return (a > b) ? a : b;
}

static double vm_f64_min(double a, double b)
{
    uint64_t ua;
    uint64_t ub;
    if (isnan(a) || isnan(b)) {
        return vm_f64_canon_nan();
    }
    if (a == b) {
        ua = vm_f64_bits(a);
        ub = vm_f64_bits(b);
        if (ua == 0x8000000000000000ULL || ub == 0x8000000000000000ULL) {
            return vm_bits_f64(0x8000000000000000ULL);
        }
        return a;
    }
    return (a < b) ? a : b;
}

static double vm_f64_max(double a, double b)
{
    uint64_t ua;
    uint64_t ub;
    if (isnan(a) || isnan(b)) {
        return vm_f64_canon_nan();
    }
    if (a == b) {
        ua = vm_f64_bits(a);
        ub = vm_f64_bits(b);
        if (ua == 0x0000000000000000ULL || ub == 0x0000000000000000ULL) {
            return vm_bits_f64(0x0000000000000000ULL);
        }
        return a;
    }
    return (a > b) ? a : b;
}

/* ================= roundTiesToEven (S3) =================
 * Hand-rolled: C round() rounds half away from zero (wrong for WASM) and
 * rint()/nearbyint() depend on the ambient rounding mode. Values with
 * |x| >= 2^23 (f32) / 2^52 (f64) are already integral (or infinite/NaN);
 * below that the truncated value fits a signed int exactly, so the
 * even/odd tie decision is exact. NaN/Inf/+-0 pass through untouched. */

static float vm_f32_nearest(float x)
{
    float t;
    float d;
    float s;
    int32_t ti;
    if (isnan(x) || x == 0.0f) {
        return x;
    }
    if (!(x > -8388608.0f && x < 8388608.0f)) { /* |x| >= 2^23 or Inf */
        return x;
    }
    t = truncf(x);
    d = x - t;
    if (d > -0.5f && d < 0.5f) {
        return t;
    }
    s = (d > 0.0f) ? 1.0f : -1.0f;
    if (d == 0.5f || d == -0.5f) {
        ti = (int32_t)t;
        if ((ti & 1) == 0) {
            return t;
        }
        return t + s;
    }
    return t + s;
}

static double vm_f64_nearest(double x)
{
    double t;
    double d;
    double s;
    int64_t ti;
    if (isnan(x) || x == 0.0) {
        return x;
    }
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) {
        return x; /* |x| >= 2^52 or Inf */
    }
    t = trunc(x);
    d = x - t;
    if (d > -0.5 && d < 0.5) {
        return t;
    }
    s = (d > 0.0) ? 1.0 : -1.0;
    if (d == 0.5 || d == -0.5) {
        ti = (int64_t)t;
        if ((ti & 1) == 0) {
            return t;
        }
        return t + s;
    }
    return t + s;
}

/* Decode a raw little-endian float immediate (f32.const/f64.const).
 * Unlike LEB128 immediates these are fixed 4/8 raw bytes. */
static wasm_trap_t vm_decode_raw_u32(wasm_frame_t *f, uint32_t *out)
{
    unsigned k;
    uint32_t u = 0u;
    if (f->code_ptr == NULL || f->code_end == NULL) {
        return WASM_TRAP_CODE_OVERRUN;
    }
    if ((uint64_t)(size_t)(f->code_end - f->code_ptr) < 4u) {
        return WASM_TRAP_CODE_OVERRUN;
    }
    for (k = 0u; k < 4u; k++) {
        u |= ((uint32_t)f->code_ptr[k]) << (8u * k);
    }
    f->code_ptr += 4;
    *out = u;
    return WASM_TRAP_OK;
}

static wasm_trap_t vm_decode_raw_u64(wasm_frame_t *f, uint64_t *out)
{
    unsigned k;
    uint64_t u = 0u;
    if (f->code_ptr == NULL || f->code_end == NULL) {
        return WASM_TRAP_CODE_OVERRUN;
    }
    if ((uint64_t)(size_t)(f->code_end - f->code_ptr) < 8u) {
        return WASM_TRAP_CODE_OVERRUN;
    }
    for (k = 0u; k < 8u; k++) {
        u |= ((uint64_t)f->code_ptr[k]) << (8u * k);
    }
    f->code_ptr += 8;
    *out = u;
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_vm_bind_memory(wasm_vm_t *vm, wasm_memory_t *mem)
{
    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    vm->mem = mem; /* NULL detaches; LOAD/STORE then trap as OOB */
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_vm_attach_code(wasm_vm_t *vm, const uint8_t *code, size_t len)
{
    wasm_frame_t *f = NULL;
    wasm_trap_t tr;
    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    tr = wasm_call_stack_top(&vm->calls, &f);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    if (len > 0u && code == NULL) {
        return WASM_TRAP_NULL;
    }
    f->code_ptr = code;
    f->code_end = (code == NULL) ? NULL : (code + len);
    return WASM_TRAP_OK;
}

/* Decode one ULEB128 memarg field (align/offset) against code_end. */
static wasm_trap_t vm_decode_u32_imm(wasm_frame_t *f, uint32_t *out)
{
    const uint8_t *p = f->code_ptr;
    wasm_err_t e = leb128_decode_u32(&p, f->code_end, out);
    if (e != WASM_OK) {
        return WASM_TRAP_CODE_OVERRUN;
    }
    f->code_ptr = p;
    return WASM_TRAP_OK;
}

/* ================= Phase 5: structured control flow ================= */

/* Decode a blocktype (MVP canonical form: single byte 0x40 void or valtype).
 * This engine executes void- and i32-valued blocks; other valtypes are
 * well-formed but unrunnable here -> INVALID_ARGUMENT. */
static wasm_trap_t vm_decode_blocktype(wasm_frame_t *f, int32_t *out)
{
    uint8_t b;
    if (f->code_ptr == NULL || f->code_end == NULL || f->code_ptr >= f->code_end) {
        return WASM_TRAP_CODE_OVERRUN;
    }
    b = *f->code_ptr++;
    if (b == 0x40u) {
        *out = (int32_t)WASM_BLOCKTYPE_VOID;
        return WASM_TRAP_OK;
    }
    if (b == (uint8_t)WASM_TYPE_I32) {
        *out = (int32_t)WASM_TYPE_I32;
        return WASM_TRAP_OK;
    }
    return WASM_TRAP_INVALID_ARGUMENT;
}

/* Immediate-skipping helpers for the forward scanner. */
static wasm_trap_t vm_skip_u32(const uint8_t **pp, const uint8_t *end)
{
    uint32_t ignored = 0u;
    if (leb128_decode_u32(pp, end, &ignored) != WASM_OK) {
        return WASM_TRAP_CODE_OVERRUN;
    }
    (void)ignored;
    return WASM_TRAP_OK;
}

static wasm_trap_t vm_skip_s32(const uint8_t **pp, const uint8_t *end)
{
    int32_t ignored = 0;
    if (leb128_decode_s32(pp, end, &ignored) != WASM_OK) {
        return WASM_TRAP_CODE_OVERRUN;
    }
    (void)ignored;
    return WASM_TRAP_OK;
}

static wasm_trap_t vm_skip_s64(const uint8_t **pp, const uint8_t *end)
{
    int64_t ignored = 0;
    if (leb128_decode_s64(pp, end, &ignored) != WASM_OK) {
        return WASM_TRAP_CODE_OVERRUN;
    }
    (void)ignored;
    return WASM_TRAP_OK;
}

/* Decode a load/store memarg (align + offset immediates); align is a
 * performance hint only and never affects correctness. */
static wasm_trap_t vm_decode_memarg(wasm_frame_t *f, uint32_t *out_offset)
{
    uint32_t align = 0u;
    wasm_trap_t tr = vm_decode_u32_imm(f, &align);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    (void)align;
    return vm_decode_u32_imm(f, out_offset);
}

/* Pop the i32 base and resolve base+offset with a wraparound guard;
 * unbound memory reports OUT_OF_BOUNDS like the other memory faults. */
static wasm_trap_t vm_resolve_ea(wasm_vm_t *vm, uint32_t offset,
                                 uint32_t *out_ea)
{
    wasm_val_t vb;
    uint64_t ea64;
    wasm_trap_t tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    ea64 = (uint64_t)(uint32_t)vb.value.i32 + (uint64_t)offset;
    if (ea64 > (uint64_t)0xFFFFFFFFu) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    if (vm->mem == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    *out_ea = (uint32_t)ea64;
    return WASM_TRAP_OK;
}

/* Resolve base+offset for a store whose value slot is already popped:
 * same wraparound and unbound-memory faults, no stack traffic. */
static wasm_trap_t vm_store_ea(wasm_vm_t *vm, int32_t base, uint32_t offset,
                               uint32_t *out_ea)
{
    uint64_t ea64 = (uint64_t)(uint32_t)base + (uint64_t)offset;
    if (ea64 > (uint64_t)0xFFFFFFFFu) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    if (vm->mem == NULL) {
        return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    }
    *out_ea = (uint32_t)ea64;
    return WASM_TRAP_OK;
}

/*
 * Forward scan from `from` for the END closing `levels` enclosing blocks.
 * Opens nest deeper; an END met at level 0 closes one enclosing block.
 * Immediates are skipped opcode-aware so 0x05/0x0B payload bytes can never
 * false-positive. First level-0 ELSE (if any) is reported via *out_else;
 * *out_end is the first byte past the final END. Unknown bytes make the
 * length unknowable -> UNKNOWN_OPCODE; running out -> CODE_OVERRUN.
 */
static wasm_trap_t vm_scan_block(const uint8_t *from, const uint8_t *end,
                                 uint32_t levels,
                                 const uint8_t **out_else,
                                 const uint8_t **out_end)
{
    const uint8_t *p = from;
    const uint8_t *found_else = NULL;
    uint32_t nest = 0u;
    uint32_t remaining;
    wasm_trap_t tr;

    if (from == NULL || end == NULL || out_else == NULL || out_end == NULL) {
        return WASM_TRAP_NULL;
    }
    if (levels == 0u) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    remaining = levels;
    while (p < end) {
        uint8_t op = *p++;
        switch (op) {
        case OP_BLOCK:
        case OP_LOOP:
        case OP_IF:
            /* Canonical single-byte blocktype (matches vm_decode_blocktype). */
            if (p >= end) {
                return WASM_TRAP_CODE_OVERRUN;
            }
            p++;
            nest++;
            break;
        case OP_ELSE:
            if (nest == 0u && found_else == NULL) {
                found_else = p;
            }
            break;
        case OP_END:
            if (nest == 0u) {
                remaining--;
                if (remaining == 0u) {
                    *out_else = found_else;
                    *out_end = p;
                    return WASM_TRAP_OK;
                }
            } else {
                nest--;
            }
            break;
        case OP_I32_CONST:
            tr = vm_skip_s32(&p, end);
            if (tr != WASM_TRAP_OK) {
                return tr;
            }
            break;
        case OP_I64_CONST:
            tr = vm_skip_s64(&p, end);
            if (tr != WASM_TRAP_OK) {
                return tr;
            }
            break;
        case OP_F32_CONST:
            /* Fixed 4-byte raw LE immediate: skip blind so embedded
             * 0x05/0x0B payload bytes can never false-positive. */
            if (p == NULL || end == NULL || p > end ||
                (uint64_t)(size_t)(end - p) < 4u) {
                return WASM_TRAP_CODE_OVERRUN;
            }
            p += 4;
            break;
        case OP_F64_CONST:
            /* Fixed 8-byte raw LE immediate (same rationale). */
            if (p == NULL || end == NULL || p > end ||
                (uint64_t)(size_t)(end - p) < 8u) {
                return WASM_TRAP_CODE_OVERRUN;
            }
            p += 8;
            break;
        case OP_LOCAL_GET:
        case OP_LOCAL_SET:
        case OP_LOCAL_TEE:
        case OP_GLOBAL_GET:
        case OP_GLOBAL_SET:
        case OP_CALL:
        case OP_CALL_INDIRECT:
        case OP_BR:
        case OP_BR_IF:
            tr = vm_skip_u32(&p, end);
            if (tr != WASM_TRAP_OK) {
                return tr;
            }
            break;
        case OP_BR_TABLE: {
            /* [count][label x count][default]: (count+1) LEBs, each >= 1
             * byte. Mirrors step validation (cap + remaining-bytes). */
            uint32_t n = 0u;
            uint32_t k;
            if (leb128_decode_u32(&p, end, &n) != WASM_OK) {
                return WASM_TRAP_CODE_OVERRUN;
            }
            if (n > (uint32_t)WASM_MAX_BR_TABLE_LABELS) {
                return WASM_TRAP_INVALID_ARGUMENT;
            }
            if ((uint64_t)(size_t)(end - p) < (uint64_t)n + 1u) {
                return WASM_TRAP_CODE_OVERRUN;
            }
            for (k = 0u; k < n + 1u; k++) {
                tr = vm_skip_u32(&p, end);
                if (tr != WASM_TRAP_OK) {
                    return tr;
                }
            }
            break;
        }
        case OP_I32_LOAD:
        case OP_I32_STORE:
        case OP_F32_LOAD:
        case OP_F32_STORE:
        case OP_F64_LOAD:
        case OP_F64_STORE:
        case OP_I64_LOAD:
        case OP_I32_LOAD8_S:
        case OP_I32_LOAD8_U:
        case OP_I32_LOAD16_S:
        case OP_I32_LOAD16_U:
        case OP_I64_LOAD8_S:
        case OP_I64_LOAD8_U:
        case OP_I64_LOAD16_S:
        case OP_I64_LOAD16_U:
        case OP_I64_LOAD32_S:
        case OP_I64_LOAD32_U:
        case OP_I64_STORE:
        case OP_I32_STORE8:
        case OP_I32_STORE16:
        case OP_I64_STORE8:
        case OP_I64_STORE16:
        case OP_I64_STORE32:
            tr = vm_skip_u32(&p, end);
            if (tr != WASM_TRAP_OK) {
                return tr;
            }
            tr = vm_skip_u32(&p, end);
            if (tr != WASM_TRAP_OK) {
                return tr;
            }
            break;
        case OP_MEMORY_SIZE:
        case OP_MEMORY_GROW:
            /* One reserved 0x00 byte (value checked at execution time). */
            if (p >= end) {
                return WASM_TRAP_CODE_OVERRUN;
            }
            p++;
            break;
        case OP_UNREACHABLE:
        case OP_NOP:
        case OP_RETURN:
        case OP_DROP:
        case OP_SELECT:
        case OP_I32_EQZ:
        case OP_I32_EQ:
        case OP_I32_NE:
        case OP_I32_LT_S:
        case OP_I32_LT_U:
        case OP_I32_GT_S:
        case OP_I32_GT_U:
        case OP_I32_LE_S:
        case OP_I32_LE_U:
        case OP_I32_GE_S:
        case OP_I32_GE_U:
        case OP_I64_EQZ:
        case OP_I64_EQ:
        case OP_I64_NE:
        case OP_I64_LT_S:
        case OP_I64_LT_U:
        case OP_I64_GT_S:
        case OP_I64_GT_U:
        case OP_I64_LE_S:
        case OP_I64_LE_U:
        case OP_I64_GE_S:
        case OP_I64_GE_U:
        case OP_I32_CLZ:
        case OP_I32_CTZ:
        case OP_I32_POPCNT:
        case OP_I32_ROTL:
        case OP_I32_ROTR:
        case OP_I64_CLZ:
        case OP_I64_CTZ:
        case OP_I64_POPCNT:
        case OP_I64_ROTL:
        case OP_I64_ROTR:
        case OP_I32_ADD:
        case OP_I32_SUB:
        case OP_I32_MUL:
        case OP_I32_DIV_S:
        case OP_I32_DIV_U:
        case OP_I32_REM_S:
        case OP_I32_REM_U:
        case OP_I32_AND:
        case OP_I32_OR:
        case OP_I32_XOR:
        case OP_I32_SHL:
        case OP_I32_SHR_S:
        case OP_I32_SHR_U:
        case OP_I64_ADD:
        case OP_I64_SUB:
        case OP_I64_MUL:
        case OP_I64_DIV_S:
        case OP_I64_DIV_U:
        case OP_I64_REM_S:
        case OP_I64_REM_U:
        case OP_I64_AND:
        case OP_I64_OR:
        case OP_I64_XOR:
        case OP_I64_SHL:
        case OP_I64_SHR_S:
        case OP_I64_SHR_U:
        case OP_F32_EQ:
        case OP_F32_NE:
        case OP_F32_LT:
        case OP_F32_GT:
        case OP_F32_LE:
        case OP_F32_GE:
        case OP_F64_EQ:
        case OP_F64_NE:
        case OP_F64_LT:
        case OP_F64_GT:
        case OP_F64_LE:
        case OP_F64_GE:
        case OP_F32_ABS:
        case OP_F32_NEG:
        case OP_F32_CEIL:
        case OP_F32_FLOOR:
        case OP_F32_TRUNC:
        case OP_F32_NEAREST:
        case OP_F32_SQRT:
        case OP_F32_ADD:
        case OP_F32_SUB:
        case OP_F32_MUL:
        case OP_F32_DIV:
        case OP_F32_MIN:
        case OP_F32_MAX:
        case OP_F32_COPYSIGN:
        case OP_F64_ABS:
        case OP_F64_NEG:
        case OP_F64_CEIL:
        case OP_F64_FLOOR:
        case OP_F64_TRUNC:
        case OP_F64_NEAREST:
        case OP_F64_SQRT:
        case OP_F64_ADD:
        case OP_F64_SUB:
        case OP_F64_MUL:
        case OP_F64_DIV:
        case OP_F64_MIN:
        case OP_F64_MAX:
        case OP_F64_COPYSIGN:
        case OP_I32_WRAP_I64:
        case OP_I64_EXTEND_I32_S:
        case OP_I64_EXTEND_I32_U:
        case OP_I32_TRUNC_F32_S:
        case OP_I32_TRUNC_F32_U:
        case OP_I32_TRUNC_F64_S:
        case OP_I32_TRUNC_F64_U:
        case OP_I64_TRUNC_F32_S:
        case OP_I64_TRUNC_F32_U:
        case OP_I64_TRUNC_F64_S:
        case OP_I64_TRUNC_F64_U:
        case OP_F32_CONVERT_I32_S:
        case OP_F32_CONVERT_I32_U:
        case OP_F32_CONVERT_I64_S:
        case OP_F32_CONVERT_I64_U:
        case OP_F32_DEMOTE_F64:
        case OP_F64_CONVERT_I32_S:
        case OP_F64_CONVERT_I32_U:
        case OP_F64_CONVERT_I64_S:
        case OP_F64_CONVERT_I64_U:
        case OP_F64_PROMOTE_F32:
        case OP_I32_REINTERPRET_F32:
        case OP_I64_REINTERPRET_F64:
        case OP_F32_REINTERPRET_I32:
        case OP_F64_REINTERPRET_I64:
            break; /* fixed one-byte instructions */
        default:
            return WASM_TRAP_UNKNOWN_OPCODE;
        }
    }
    return WASM_TRAP_CODE_OVERRUN;
}

/*
 * Branch to control[depth-1-label]. The operand stack is cut back to the
 * target's entry height, carrying one result value across when the target
 * block is valued. LOOP targets rewind to body_start and keep their control
 * entry; BLOCK/IF targets jump past the target's END and drop it.
 */
static wasm_trap_t vm_do_branch(wasm_vm_t *vm, wasm_frame_t *f, uint32_t label)
{
    wasm_control_stack_t *cs = &f->ctrl;
    wasm_control_frame_t *tgt;
    uint32_t depth = wasm_control_stack_depth(cs);
    wasm_trap_t tr;

    if (label >= depth) {
        return WASM_TRAP_INVALID_BLOCK_LABEL;
    }
    tgt = &cs->items[depth - 1u - label];

    if (tgt->blocktype == (int32_t)WASM_BLOCKTYPE_VOID) {
        if (wasm_stack_depth(&vm->operands) < tgt->stack_height) {
            return WASM_TRAP_STACK_UNDERFLOW;
        }
        vm->operands.top = tgt->stack_height;
    } else {
        wasm_val_t res;
        tr = wasm_stack_pop(&vm->operands, &res);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (tgt->stack_height > wasm_stack_depth(&vm->operands)) {
            (void)wasm_stack_push(&vm->operands, res);
            return WASM_TRAP_STACK_UNDERFLOW;
        }
        vm->operands.top = tgt->stack_height;
        tr = wasm_stack_push(&vm->operands, res);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
    }

    if (tgt->kind == WASM_BLOCK_TYPE_LOOP) {
        f->code_ptr = tgt->body_start;
        cs->depth = depth - label; /* retain target, drop entries above it */
    } else {
        const uint8_t *else_pos = NULL;
        const uint8_t *end_pos = NULL;
        tr = vm_scan_block(f->code_ptr, f->code_end, label + 1u, &else_pos, &end_pos);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        (void)else_pos;
        f->code_ptr = end_pos;
        cs->depth = depth - label - 1u; /* drop target and everything above */
    }
    return WASM_TRAP_OK;
}

/* Invoke a host callee: pop args (last first), call, push results. */
static wasm_trap_t vm_invoke_host(wasm_vm_t *vm, const wasm_func_desc_t *d)
{
    wasm_val_t argv[WASM_MAX_LOCALS_PER_FRAME];
    wasm_val_t retv[1];
    uint32_t i;
    wasm_trap_t tr;
    if (d->host_cb == NULL) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    if (d->param_count > (uint32_t)WASM_MAX_LOCALS_PER_FRAME ||
        d->return_count > 1u) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    if (wasm_stack_depth(&vm->operands) < d->param_count) {
        return WASM_TRAP_STACK_UNDERFLOW;
    }
    for (i = 0u; i < d->param_count; i++) {
        tr = wasm_stack_pop(&vm->operands, &argv[d->param_count - 1u - i]);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
    }
    retv[0].type = WASM_TYPE_I32;
    retv[0].value.i32 = 0;
    tr = d->host_cb(vm, argv, d->param_count, retv, d->return_count);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    for (i = 0u; i < d->return_count; i++) {
        tr = wasm_stack_push(&vm->operands, retv[i]);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
    }
    return WASM_TRAP_OK;
}

/* Enter a WASM-code callee: new frame plus borrowed code window. */
static wasm_trap_t vm_enter_callee(wasm_vm_t *vm, uint32_t idx,
                                   const wasm_func_desc_t *d)
{
    wasm_trap_t tr = wasm_vm_enter_function(vm, idx, d->param_count,
                                            d->local_count,
                                            d->return_count);
    if (tr != WASM_TRAP_OK) {
        return tr;
    }
    return wasm_vm_attach_code(vm, d->code, d->code_len);
}

/*
 * Verify a call_indirect target against the type-section entry `typeidx:
 * param AND return counts plus every value type must be equal. Targets
 * without recorded signatures (directly-defined or untyped host funcs)
 * and missing/unparseable type bytes fail closed with a mismatch.
 */
static wasm_trap_t vm_check_indirect_sig(wasm_vm_t *vm,
                                         const wasm_func_desc_t *d,
                                         uint32_t typeidx)
{
    const uint8_t *p, *end;
    uint32_t count, t, npc, nrc, k;
    if (!d->has_sig) {
        return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
    }
    if (vm->typesec == NULL || vm->typesec_len == 0u) {
        return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
    }
    p = vm->typesec;
    end = vm->typesec + vm->typesec_len;
    if (leb128_decode_u32(&p, end, &count) != WASM_OK || typeidx >= count) {
        return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
    }
    for (t = 0u; t <= typeidx; t++) {
        uint8_t tag;
        if ((uint64_t)(size_t)(end - p) < 1u) {
            return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
        }
        tag = *p++;
        if (tag != 0x60u) {
            return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
        }
        if (leb128_decode_u32(&p, end, &npc) != WASM_OK) {
            return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
        }
        if (t == typeidx) {
            if (npc != d->param_count) {
                return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
            }
            if (npc > (uint32_t)WASM_MAX_LOCALS_PER_FRAME) {
                return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
            }
            for (k = 0u; k < npc; k++) {
                uint8_t vt;
                if ((uint64_t)(size_t)(end - p) < 1u) {
                    return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
                }
                vt = *p++;
                if (vt != d->param_types[k]) {
                    return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
                }
            }
        } else {
            uint32_t j;
            for (j = 0u; j < npc; j++) {
                if ((uint64_t)(size_t)(end - p) < 1u) {
                    return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
                }
                p++;
            }
        }
        if (leb128_decode_u32(&p, end, &nrc) != WASM_OK) {
            return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
        }
        if (t == typeidx) {
            uint8_t vt;
            if (nrc != d->return_count) {
                return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
            }
            if (nrc > 1u) {
                return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
            }
            if (nrc == 1u) {
                if ((uint64_t)(size_t)(end - p) < 1u) {
                    return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
                }
                vt = *p++;
                if (vt != d->return_type) {
                    return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
                }
            }
            return WASM_TRAP_OK;
        } else {
            uint32_t j;
            for (j = 0u; j < nrc; j++) {
                if ((uint64_t)(size_t)(end - p) < 1u) {
                    return WASM_TRAP_INDIRECT_TYPE_MISMATCH;
                }
                p++;
            }
        }
    }
    return WASM_TRAP_INDIRECT_TYPE_MISMATCH; /* unreachable */
}

wasm_trap_t wasm_vm_step(wasm_vm_t *vm)
{
    wasm_frame_t *f = NULL;
    wasm_trap_t tr;
    uint8_t op;

    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    tr = wasm_call_stack_top(&vm->calls, &f);
    if (tr != WASM_TRAP_OK) {
        return tr; /* no active frame */
    }
    if (f->code_ptr == NULL || f->code_end == NULL || f->code_ptr >= f->code_end) {
        return WASM_TRAP_CODE_OVERRUN;
    }
    op = *f->code_ptr++;

    switch (op) {
    case OP_UNREACHABLE:
        return WASM_TRAP_UNREACHABLE;

    case OP_BLOCK: {
        int32_t bt = 0;
        wasm_control_frame_t cf;
        tr = vm_decode_blocktype(f, &bt);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        cf.kind = WASM_BLOCK_TYPE_BLOCK;
        cf.blocktype = bt;
        cf.stack_height = wasm_stack_depth(&vm->operands);
        cf.body_start = NULL;
        return wasm_control_stack_push(&f->ctrl, &cf);
    }

    case OP_LOOP: {
        int32_t bt = 0;
        wasm_control_frame_t cf;
        tr = vm_decode_blocktype(f, &bt);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        cf.kind = WASM_BLOCK_TYPE_LOOP;
        cf.blocktype = bt;
        cf.stack_height = wasm_stack_depth(&vm->operands);
        cf.body_start = f->code_ptr;
        return wasm_control_stack_push(&f->ctrl, &cf);
    }

    case OP_IF: {
        int32_t bt = 0;
        wasm_val_t cv;
        wasm_control_frame_t cf;
        tr = vm_decode_blocktype(f, &bt);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &cv);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (cv.value.i32 != 0) {
            cf.kind = WASM_BLOCK_TYPE_IF;
            cf.blocktype = bt;
            cf.stack_height = wasm_stack_depth(&vm->operands);
            cf.body_start = NULL;
            return wasm_control_stack_push(&f->ctrl, &cf);
        }
        {
            const uint8_t *else_pos = NULL;
            const uint8_t *end_pos = NULL;
            tr = vm_scan_block(f->code_ptr, f->code_end, 1u, &else_pos, &end_pos);
            if (tr != WASM_TRAP_OK) {
                return tr;
            }
            if (else_pos != NULL) {
                f->code_ptr = else_pos;
                cf.kind = WASM_BLOCK_TYPE_IF;
                cf.blocktype = bt;
                cf.stack_height = wasm_stack_depth(&vm->operands);
                cf.body_start = NULL;
                return wasm_control_stack_push(&f->ctrl, &cf);
            }
            f->code_ptr = end_pos;
            return WASM_TRAP_OK;
        }
    }

    case OP_ELSE: {
        wasm_control_frame_t *top = NULL;
        const uint8_t *else_pos = NULL;
        const uint8_t *end_pos = NULL;
        tr = wasm_control_stack_top(&f->ctrl, &top);
        if (tr != WASM_TRAP_OK || top->kind != WASM_BLOCK_TYPE_IF) {
            return WASM_TRAP_ELSE_WITHOUT_IF;
        }
        tr = vm_scan_block(f->code_ptr, f->code_end, 1u, &else_pos, &end_pos);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        (void)else_pos;
        f->code_ptr = end_pos;
        return wasm_control_stack_pop(&f->ctrl, NULL);
    }

    case OP_END: {
        /* A block/loop/if end pops one control entry. A function-level
         * end (empty control stack) is the MVP function-end marker: a
         * no-op here, after which the code window is consumed and the
         * caller (run loop / CLI step loop) exits the frame. */
        wasm_control_frame_t dropped;
        tr = wasm_control_stack_pop(&f->ctrl, &dropped);
        (void)dropped;
        if (tr == WASM_TRAP_CONTROL_STACK_UNDERFLOW) {
            return WASM_TRAP_OK;
        }
        return tr;
    }

    case OP_BR: {
        uint32_t label = 0u;
        tr = vm_decode_u32_imm(f, &label);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_do_branch(vm, f, label);
    }

    case OP_BR_IF: {
        uint32_t label = 0u;
        wasm_val_t cv;
        tr = vm_decode_u32_imm(f, &label);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &cv);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (cv.value.i32 != 0) {
            return vm_do_branch(vm, f, label);
        }
        return WASM_TRAP_OK;
    }

    case OP_BR_TABLE: {
        /* [count][label x count][default]: pop unsigned i32 index; take
         * labels[i] if i < count else the default. Zero allocation: the
         * walk keeps only the selected label. count > remaining bytes is
         * malformed (each label costs >= 1 byte); count above
         * WASM_MAX_BR_TABLE_LABELS is rejected without reading further. */
        uint32_t count = 0u;
        uint32_t i;
        uint32_t idx;
        uint32_t sel = 0u;
        wasm_val_t cv;
        const uint8_t *p;
        tr = vm_decode_u32_imm(f, &count);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (count > (uint32_t)WASM_MAX_BR_TABLE_LABELS) {
            return WASM_TRAP_INVALID_ARGUMENT;
        }
        if (f->code_ptr == NULL || f->code_end == NULL ||
            (uint64_t)(size_t)(f->code_end - f->code_ptr) <
            (uint64_t)count + 1u) {
            return WASM_TRAP_CODE_OVERRUN;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &cv);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        idx = (uint32_t)cv.value.i32; /* unsigned: 0xFFFFFFFF > any count */
        p = f->code_ptr;
        for (i = 0u; i < count; i++) {
            uint32_t lab = 0u;
            if (leb128_decode_u32(&p, f->code_end, &lab) != WASM_OK) {
                return WASM_TRAP_CODE_OVERRUN;
            }
            if (i == idx) {
                sel = lab;
            }
        }
        {
            uint32_t dflt = 0u;
            if (leb128_decode_u32(&p, f->code_end, &dflt) != WASM_OK) {
                return WASM_TRAP_CODE_OVERRUN;
            }
            if (idx >= count) {
                sel = dflt;
            }
        }
        f->code_ptr = p;
        return vm_do_branch(vm, f, sel);
    }

    case OP_NOP:
        return WASM_TRAP_OK;

    case OP_DROP: {
        /* MVP parametric drop: pop and discard one value. Empty stack
         * traps; the pop itself reports the reason (underflow). */
        wasm_val_t tmp;
        return wasm_stack_pop(&vm->operands, &tmp);
    }

    case OP_SELECT: {
        /* MVP untyped select: stack [v1, v2, cond] (cond on top) ->
         * v1 if cond != 0 else v2. v1/v2 must share a type; failures
         * restore the popped prefix so a trapped select leaves the stack
         * untouched (push-backs always fit: depth only shrank). */
        wasm_val_t c;
        wasm_val_t v1;
        wasm_val_t v2;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &c);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop(&vm->operands, &v2);
        if (tr != WASM_TRAP_OK) {
            (void)wasm_stack_push(&vm->operands, c);
            return tr;
        }
        tr = wasm_stack_pop(&vm->operands, &v1);
        if (tr != WASM_TRAP_OK) {
            (void)wasm_stack_push(&vm->operands, v2);
            (void)wasm_stack_push(&vm->operands, c);
            return tr;
        }
        if (v1.type != v2.type) {
            (void)wasm_stack_push(&vm->operands, v1);
            (void)wasm_stack_push(&vm->operands, v2);
            (void)wasm_stack_push(&vm->operands, c);
            return WASM_TRAP_TYPE_MISMATCH;
        }
        if (c.value.i32 != 0) {
            return wasm_stack_push(&vm->operands, v1);
        }
        return wasm_stack_push(&vm->operands, v2);
    }

    case OP_RETURN: {
        wasm_frame_t *top = NULL;
        tr = wasm_call_stack_top(&vm->calls, &top);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return wasm_vm_exit_function(vm, top->return_count);
    }

    case OP_CALL: {
        uint32_t idx = 0u;
        const wasm_func_desc_t *d;
        tr = vm_decode_u32_imm(f, &idx);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (idx >= (uint32_t)WASM_MAX_FUNCTIONS) {
            return WASM_TRAP_INVALID_ARGUMENT;
        }
        d = &vm->funcs[idx];
        if (!d->valid) {
            return WASM_TRAP_INVALID_ARGUMENT;
        }
        if (d->is_host) {
            return vm_invoke_host(vm, d);
        }
        return vm_enter_callee(vm, idx, d);
    }

    case OP_CALL_INDIRECT: {
        /* [typeidx]: pop index, bounds-check the table, resolve the slot,
         * verify the signature element-by-element, then call like OP_CALL.
         * The signature check precedes argument popping (fail closed). */
        uint32_t typeidx = 0u;
        uint32_t index;
        uint32_t slot;
        const wasm_func_desc_t *d;
        wasm_val_t cv;
        tr = vm_decode_u32_imm(f, &typeidx);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &cv);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        index = (uint32_t)cv.value.i32;
        if (index >= vm->table_size) {
            return WASM_TRAP_TABLE_OUT_OF_BOUNDS;
        }
        slot = vm->table[index];
        if (slot == (uint32_t)WASM_TABLE_EMPTY ||
            slot >= (uint32_t)WASM_MAX_FUNCTIONS) {
            return WASM_TRAP_UNDEFINED_ELEMENT;
        }
        d = &vm->funcs[slot];
        if (!d->valid) {
            return WASM_TRAP_UNDEFINED_ELEMENT;
        }
        tr = vm_check_indirect_sig(vm, d, typeidx);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (d->is_host) {
            return vm_invoke_host(vm, d);
        }
        return vm_enter_callee(vm, slot, d);
    }

    case OP_I32_CONST: {
        const uint8_t *p = f->code_ptr;
        int32_t v = 0;
        if (leb128_decode_s32(&p, f->code_end, &v) != WASM_OK) {
            return WASM_TRAP_CODE_OVERRUN;
        }
        f->code_ptr = p;
        return vm_push_i32(vm, v);
    }

    case OP_I64_CONST: {
        const uint8_t *p = f->code_ptr;
        int64_t v = 0;
        if (leb128_decode_s64(&p, f->code_end, &v) != WASM_OK) {
            return WASM_TRAP_CODE_OVERRUN;
        }
        f->code_ptr = p;
        return vm_push_i64(vm, v);
    }

    case OP_F32_CONST: {
        uint32_t u = 0u;
        tr = vm_decode_raw_u32(f, &u);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f32(vm, vm_bits_f32(u)); /* bit-exact, NaN-safe */
    }

    case OP_F64_CONST: {
        uint64_t u = 0u;
        tr = vm_decode_raw_u64(f, &u);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f64(vm, vm_bits_f64(u)); /* bit-exact, NaN-safe */
    }

    case OP_LOCAL_GET:
    case OP_LOCAL_SET:
    case OP_LOCAL_TEE: {
        uint32_t idx = 0u;
        tr = vm_decode_u32_imm(f, &idx);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_LOCAL_GET) {
            return wasm_vm_local_get(vm, idx);
        }
        if (op == OP_LOCAL_SET) {
            return wasm_vm_local_set(vm, idx);
        }
        return wasm_vm_local_tee(vm, idx);
    }

    case OP_GLOBAL_GET: {
        uint32_t idx = 0u;
        tr = vm_decode_u32_imm(f, &idx);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (idx >= vm->nglobals || !vm->globals[idx].valid) {
            return WASM_TRAP_INVALID_ARGUMENT;
        }
        return wasm_stack_push(&vm->operands, vm->globals[idx].value);
    }

    case OP_GLOBAL_SET: {
        uint32_t idx = 0u;
        wasm_val_t v;
        tr = vm_decode_u32_imm(f, &idx);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (idx >= vm->nglobals || !vm->globals[idx].valid) {
            return WASM_TRAP_INVALID_ARGUMENT;
        }
        if (!vm->globals[idx].mut) {
            return WASM_TRAP_GLOBAL_IMMUTABLE;
        }
        tr = wasm_stack_pop_typed(&vm->operands,
                                  vm->globals[idx].value.type, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        vm->globals[idx].value = v;
        return WASM_TRAP_OK;
    }

    case OP_I32_ADD:
    case OP_I32_SUB:
    case OP_I32_MUL:
    case OP_I32_AND:
    case OP_I32_OR:
    case OP_I32_XOR: {
        int32_t a = 0, b = 0;
        uint32_t r;
        tr = vm_pop2_i32(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_I32_ADD) {
            r = (uint32_t)a + (uint32_t)b;
        } else if (op == OP_I32_SUB) {
            r = (uint32_t)a - (uint32_t)b;
        } else if (op == OP_I32_MUL) {
            r = (uint32_t)a * (uint32_t)b;
        } else if (op == OP_I32_AND) {
            r = (uint32_t)a & (uint32_t)b;
        } else if (op == OP_I32_OR) {
            r = (uint32_t)a | (uint32_t)b;
        } else {
            r = (uint32_t)a ^ (uint32_t)b;
        }
        return vm_push_i32(vm, vm_u2s(r));
    }

    case OP_I32_DIV_S:
    case OP_I32_DIV_U:
    case OP_I32_REM_S:
    case OP_I32_REM_U: {
        int32_t a = 0, b = 0;
        tr = vm_pop2_i32(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (b == 0) {
            return WASM_TRAP_DIV_BY_ZERO;
        }
        if (op == OP_I32_DIV_S) {
            if (a == INT32_MIN && b == -1) {
                return WASM_TRAP_INTEGER_OVERFLOW;
            }
            return vm_push_i32(vm, a / b);
        }
        if (op == OP_I32_DIV_U) {
            return vm_push_i32(vm, vm_u2s((uint32_t)a / (uint32_t)b));
        }
        if (op == OP_I32_REM_S) {
            if (a == INT32_MIN && b == -1) {
                return vm_push_i32(vm, 0); /* MVP: MIN%-1 is 0, not a trap */
            }
            return vm_push_i32(vm, a % b);
        }
        return vm_push_i32(vm, vm_u2s((uint32_t)a % (uint32_t)b));
    }

    case OP_I32_SHL:
    case OP_I32_SHR_S:
    case OP_I32_SHR_U: {
        int32_t a = 0, b = 0;
        uint32_t n;
        tr = vm_pop2_i32(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        n = (uint32_t)b & 31u; /* WASM masks the count; also kills C UB */
        if (op == OP_I32_SHL) {
            return vm_push_i32(vm, vm_u2s((uint32_t)a << n));
        }
        if (op == OP_I32_SHR_U) {
            return vm_push_i32(vm, vm_u2s((uint32_t)a >> n));
        }
        { /* SHR_S: portable arithmetic shift (C >> on negatives is impl-defined). */
            uint32_t logical = (uint32_t)a >> n;
            uint32_t r;
            if (n == 0u || a >= 0) {
                r = logical;
            } else {
                r = logical | ((uint32_t)(~0u) << (32u - n));
            }
            return vm_push_i32(vm, vm_u2s(r));
        }
    }

    case OP_I32_CLZ:
    case OP_I32_CTZ:
    case OP_I32_POPCNT: {
        /* Bit counts over the raw u32 pattern. clz/ctz of 0 give 32.
         * Plain unsigned shifts only: no C UB, no builtins. */
        wasm_val_t v;
        uint32_t x;
        uint32_t r = 0u;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        memcpy(&x, &v.value.i32, sizeof(x));
        if (op == OP_I32_CLZ) {
            if (x == 0u) {
                r = 32u;
            } else {
                while ((x & 0x80000000u) == 0u) {
                    x <<= 1u;
                    r++;
                }
            }
        } else if (op == OP_I32_CTZ) {
            if (x == 0u) {
                r = 32u;
            } else {
                while ((x & 1u) == 0u) {
                    x >>= 1u;
                    r++;
                }
            }
        } else {
            while (x != 0u) {
                r += x & 1u;
                x >>= 1u;
            }
        }
        return vm_push_i32(vm, vm_u2s(r));
    }

    case OP_I32_ROTL:
    case OP_I32_ROTR: {
        /* Rotate by (count mod 32); a zero count is the identity (shifting
         * by 32 would be C UB, so it is special-cased, not masked away). */
        int32_t a = 0, b = 0;
        uint32_t x;
        uint32_t n;
        uint32_t r;
        tr = vm_pop2_i32(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        memcpy(&x, &a, sizeof(x));
        n = (uint32_t)b & 31u;
        if (n == 0u) {
            r = x;
        } else if (op == OP_I32_ROTL) {
            r = (x << n) | (x >> (32u - n));
        } else {
            r = (x >> n) | (x << (32u - n));
        }
        return vm_push_i32(vm, vm_u2s(r));
    }

    case OP_I64_ADD:
    case OP_I64_SUB:
    case OP_I64_MUL:
    case OP_I64_AND:
    case OP_I64_OR:
    case OP_I64_XOR: {
        int64_t a = 0, b = 0;
        uint64_t r;
        tr = vm_pop2_i64(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_I64_ADD) {
            r = (uint64_t)a + (uint64_t)b;
        } else if (op == OP_I64_SUB) {
            r = (uint64_t)a - (uint64_t)b;
        } else if (op == OP_I64_MUL) {
            r = (uint64_t)a * (uint64_t)b;
        } else if (op == OP_I64_AND) {
            r = (uint64_t)a & (uint64_t)b;
        } else if (op == OP_I64_OR) {
            r = (uint64_t)a | (uint64_t)b;
        } else {
            r = (uint64_t)a ^ (uint64_t)b;
        }
        return vm_push_i64(vm, vm_u2s64(r));
    }

    case OP_I64_DIV_S:
    case OP_I64_DIV_U:
    case OP_I64_REM_S:
    case OP_I64_REM_U: {
        int64_t a = 0, b = 0;
        tr = vm_pop2_i64(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (b == 0) {
            return WASM_TRAP_DIV_BY_ZERO;
        }
        if (op == OP_I64_DIV_S) {
            if (a == INT64_MIN && b == -1) {
                return WASM_TRAP_INTEGER_OVERFLOW;
            }
            return vm_push_i64(vm, a / b);
        }
        if (op == OP_I64_DIV_U) {
            return vm_push_i64(vm, vm_u2s64((uint64_t)a / (uint64_t)b));
        }
        if (op == OP_I64_REM_S) {
            if (a == INT64_MIN && b == -1) {
                return vm_push_i64(vm, 0); /* MVP: MIN%-1 is 0, not a trap */
            }
            return vm_push_i64(vm, a % b);
        }
        return vm_push_i64(vm, vm_u2s64((uint64_t)a % (uint64_t)b));
    }

    case OP_I64_SHL:
    case OP_I64_SHR_S:
    case OP_I64_SHR_U: {
        int64_t a = 0, b = 0;
        uint64_t n;
        tr = vm_pop2_i64(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        n = (uint64_t)b & 63u; /* WASM masks the count; also kills C UB */
        if (op == OP_I64_SHL) {
            return vm_push_i64(vm, vm_u2s64((uint64_t)a << n));
        }
        if (op == OP_I64_SHR_U) {
            return vm_push_i64(vm, vm_u2s64((uint64_t)a >> n));
        }
        { /* SHR_S: portable arithmetic shift (C >> on negatives is impl-defined). */
            uint64_t logical = (uint64_t)a >> n;
            uint64_t r;
            if (n == 0u || a >= 0) {
                r = logical;
            } else {
                r = logical | ((uint64_t)(~0ULL) << (64u - n));
            }
            return vm_push_i64(vm, vm_u2s64(r));
        }
    }

    case OP_I64_CLZ:
    case OP_I64_CTZ:
    case OP_I64_POPCNT: {
        /* Bit counts over the raw u64 pattern. clz/ctz of 0 give 64.
         * Plain unsigned shifts only: no C UB, no builtins. */
        wasm_val_t v;
        uint64_t x = 0u;
        uint64_t r = 0u;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        memcpy(&x, &v.value.i64, sizeof(x));
        if (op == OP_I64_CLZ) {
            if (x == 0u) {
                r = 64u;
            } else {
                while ((x & 0x8000000000000000uLL) == 0u) {
                    x <<= 1u;
                    r++;
                }
            }
        } else if (op == OP_I64_CTZ) {
            if (x == 0u) {
                r = 64u;
            } else {
                while ((x & 1u) == 0u) {
                    x >>= 1u;
                    r++;
                }
            }
        } else {
            while (x != 0u) {
                r += x & 1u;
                x >>= 1u;
            }
        }
        return vm_push_i64(vm, vm_u2s64(r));
    }

    case OP_I64_ROTL:
    case OP_I64_ROTR: {
        /* Rotate by (count mod 64); a zero count is the identity
         * (shifting by 64 would be C UB, so it is special-cased). */
        int64_t a = 0, b = 0;
        uint64_t x = 0u;
        uint64_t n;
        uint64_t r;
        tr = vm_pop2_i64(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        memcpy(&x, &a, sizeof(x));
        n = (uint64_t)b & 63u;
        if (n == 0u) {
            r = x;
        } else if (op == OP_I64_ROTL) {
            r = (x << n) | (x >> (64u - n));
        } else {
            r = (x >> n) | (x << (64u - n));
        }
        return vm_push_i64(vm, vm_u2s64(r));
    }

    /* ================= Float arithmetic (IEEE-754; S1: float division by
     * zero is NOT a trap — the C operator yields +-Inf/NaN per IEEE) ====== */

    case OP_F32_ADD:
    case OP_F32_SUB:
    case OP_F32_MUL:
    case OP_F32_DIV: {
        float a = 0.0f, b = 0.0f;
        tr = vm_pop2_f32(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_F32_ADD) {
            return vm_push_f32(vm, a + b);
        }
        if (op == OP_F32_SUB) {
            return vm_push_f32(vm, a - b);
        }
        if (op == OP_F32_MUL) {
            return vm_push_f32(vm, a * b);
        }
        return vm_push_f32(vm, a / b);
    }

    case OP_F64_ADD:
    case OP_F64_SUB:
    case OP_F64_MUL:
    case OP_F64_DIV: {
        double a = 0.0, b = 0.0;
        tr = vm_pop2_f64(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_F64_ADD) {
            return vm_push_f64(vm, a + b);
        }
        if (op == OP_F64_SUB) {
            return vm_push_f64(vm, a - b);
        }
        if (op == OP_F64_MUL) {
            return vm_push_f64(vm, a * b);
        }
        return vm_push_f64(vm, a / b);
    }

    case OP_F32_SQRT: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f32(vm, sqrtf(v.value.f32)); /* S5: sqrt(neg) = NaN */
    }

    case OP_F64_SQRT: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f64(vm, sqrt(v.value.f64)); /* S5: sqrt(neg) = NaN */
    }

    case OP_F32_MIN:
    case OP_F32_MAX: {
        float a = 0.0f, b = 0.0f;
        tr = vm_pop2_f32(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_F32_MIN) {
            return vm_push_f32(vm, vm_f32_min(a, b));
        }
        return vm_push_f32(vm, vm_f32_max(a, b));
    }

    case OP_F64_MIN:
    case OP_F64_MAX: {
        double a = 0.0, b = 0.0;
        tr = vm_pop2_f64(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_F64_MIN) {
            return vm_push_f64(vm, vm_f64_min(a, b));
        }
        return vm_push_f64(vm, vm_f64_max(a, b));
    }

    /* S6: sign-bit ops touch only bit patterns, never float values. */
    case OP_F32_ABS:
    case OP_F32_NEG: {
        wasm_val_t v;
        uint32_t u;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        u = vm_f32_bits(v.value.f32);
        if (op == OP_F32_ABS) {
            u &= (uint32_t)0x7FFFFFFFu;
        } else {
            u ^= (uint32_t)0x80000000u;
        }
        return vm_push_f32(vm, vm_bits_f32(u));
    }

    case OP_F64_ABS:
    case OP_F64_NEG: {
        wasm_val_t v;
        uint64_t u;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        u = vm_f64_bits(v.value.f64);
        if (op == OP_F64_ABS) {
            u &= (uint64_t)0x7FFFFFFFFFFFFFFFULL;
        } else {
            u ^= (uint64_t)0x8000000000000000ULL;
        }
        return vm_push_f64(vm, vm_bits_f64(u));
    }

    case OP_F32_CEIL:
    case OP_F32_FLOOR:
    case OP_F32_TRUNC: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_F32_CEIL) {
            return vm_push_f32(vm, ceilf(v.value.f32));
        }
        if (op == OP_F32_FLOOR) {
            return vm_push_f32(vm, floorf(v.value.f32));
        }
        return vm_push_f32(vm, truncf(v.value.f32));
    }

    case OP_F64_CEIL:
    case OP_F64_FLOOR:
    case OP_F64_TRUNC: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_F64_CEIL) {
            return vm_push_f64(vm, ceil(v.value.f64));
        }
        if (op == OP_F64_FLOOR) {
            return vm_push_f64(vm, floor(v.value.f64));
        }
        return vm_push_f64(vm, trunc(v.value.f64));
    }

    case OP_F32_NEAREST: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f32(vm, vm_f32_nearest(v.value.f32));
    }

    case OP_F64_NEAREST: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f64(vm, vm_f64_nearest(v.value.f64));
    }

    case OP_F32_COPYSIGN: {
        float a = 0.0f, b = 0.0f;
        uint32_t ua;
        uint32_t ub;
        tr = vm_pop2_f32(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        ua = vm_f32_bits(a);
        ub = vm_f32_bits(b);
        ua = (ua & (uint32_t)0x7FFFFFFFu) | (ub & (uint32_t)0x80000000u);
        return vm_push_f32(vm, vm_bits_f32(ua));
    }

    case OP_F64_COPYSIGN: {
        double a = 0.0, b = 0.0;
        uint64_t ua;
        uint64_t ub;
        tr = vm_pop2_f64(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        ua = vm_f64_bits(a);
        ub = vm_f64_bits(b);
        ua = (ua & (uint64_t)0x7FFFFFFFFFFFFFFFULL) |
             (ub & (uint64_t)0x8000000000000000ULL);
        return vm_push_f64(vm, vm_bits_f64(ua));
    }

    /* Float comparisons: C relational semantics already match WASM
     * (any ordered comparison with NaN is false; ne is true). Result i32. */
    case OP_F32_EQ:
    case OP_F32_NE:
    case OP_F32_LT:
    case OP_F32_GT:
    case OP_F32_LE:
    case OP_F32_GE: {
        float a = 0.0f, b = 0.0f;
        int cond = 0;
        tr = vm_pop2_f32(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        switch (op) {
        case OP_F32_EQ: cond = (a == b); break;
        case OP_F32_NE: cond = (a != b); break;
        case OP_F32_LT: cond = (a < b); break;
        case OP_F32_GT: cond = (a > b); break;
        case OP_F32_LE: cond = (a <= b); break;
        default:        cond = (a >= b); break; /* OP_F32_GE */
        }
        return vm_push_bool(vm, cond);
    }

    case OP_F64_EQ:
    case OP_F64_NE:
    case OP_F64_LT:
    case OP_F64_GT:
    case OP_F64_LE:
    case OP_F64_GE: {
        double a = 0.0, b = 0.0;
        int cond = 0;
        tr = vm_pop2_f64(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        switch (op) {
        case OP_F64_EQ: cond = (a == b); break;
        case OP_F64_NE: cond = (a != b); break;
        case OP_F64_LT: cond = (a < b); break;
        case OP_F64_GT: cond = (a > b); break;
        case OP_F64_LE: cond = (a <= b); break;
        default:        cond = (a >= b); break; /* OP_F64_GE */
        }
        return vm_push_bool(vm, cond);
    }

    /* ================= Truncating float->int conversions (S4) ================
     * NaN -> INVALID_CONVERSION; out-of-range -> INTEGER_OVERFLOW. Boundary
     * constants are exact powers of two: comparisons below use < low (low
     * itself is valid) and >= high (high itself already overflows). The C
     * cast is only reached in-range, so it is always defined. */

    case OP_I32_TRUNC_F32_S: {
        wasm_val_t v;
        float x;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        x = v.value.f32;
        if (isnan(x)) {
            return WASM_TRAP_INVALID_CONVERSION;
        }
        if (x < -2147483648.0f || x >= 2147483648.0f) {
            return WASM_TRAP_INTEGER_OVERFLOW;
        }
        return vm_push_i32(vm, (int32_t)x);
    }

    case OP_I32_TRUNC_F32_U: {
        wasm_val_t v;
        float x;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        x = v.value.f32;
        if (isnan(x)) {
            return WASM_TRAP_INVALID_CONVERSION;
        }
        if (x < 0.0f || x >= 4294967296.0f) {
            return WASM_TRAP_INTEGER_OVERFLOW;
        }
        return vm_push_i32(vm, vm_u2s((uint32_t)x));
    }

    case OP_I32_TRUNC_F64_S: {
        wasm_val_t v;
        double x;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        x = v.value.f64;
        if (isnan(x)) {
            return WASM_TRAP_INVALID_CONVERSION;
        }
        if (x < -2147483648.0 || x >= 2147483648.0) {
            return WASM_TRAP_INTEGER_OVERFLOW;
        }
        return vm_push_i32(vm, (int32_t)x);
    }

    case OP_I32_TRUNC_F64_U: {
        wasm_val_t v;
        double x;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        x = v.value.f64;
        if (isnan(x)) {
            return WASM_TRAP_INVALID_CONVERSION;
        }
        if (x < 0.0 || x >= 4294967296.0) {
            return WASM_TRAP_INTEGER_OVERFLOW;
        }
        return vm_push_i32(vm, vm_u2s((uint32_t)x));
    }

    case OP_I64_TRUNC_F32_S: {
        wasm_val_t v;
        float x;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        x = v.value.f32;
        if (isnan(x)) {
            return WASM_TRAP_INVALID_CONVERSION;
        }
        if (x < -9223372036854775808.0f || x >= 9223372036854775808.0f) {
            return WASM_TRAP_INTEGER_OVERFLOW;
        }
        return vm_push_i64(vm, (int64_t)x);
    }

    case OP_I64_TRUNC_F32_U: {
        wasm_val_t v;
        float x;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        x = v.value.f32;
        if (isnan(x)) {
            return WASM_TRAP_INVALID_CONVERSION;
        }
        if (x < 0.0f || x >= 18446744073709551616.0f) {
            return WASM_TRAP_INTEGER_OVERFLOW;
        }
        return vm_push_i64(vm, vm_u2s64((uint64_t)x));
    }

    case OP_I64_TRUNC_F64_S: {
        wasm_val_t v;
        double x;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        x = v.value.f64;
        if (isnan(x)) {
            return WASM_TRAP_INVALID_CONVERSION;
        }
        if (x < -9223372036854775808.0 || x >= 9223372036854775808.0) {
            return WASM_TRAP_INTEGER_OVERFLOW;
        }
        return vm_push_i64(vm, (int64_t)x);
    }

    case OP_I64_TRUNC_F64_U: {
        wasm_val_t v;
        double x;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        x = v.value.f64;
        if (isnan(x)) {
            return WASM_TRAP_INVALID_CONVERSION;
        }
        if (x < 0.0 || x >= 18446744073709551616.0) {
            return WASM_TRAP_INTEGER_OVERFLOW;
        }
        return vm_push_i64(vm, vm_u2s64((uint64_t)x));
    }

    /* int->float: C casts round per the ambient (default ties-to-even) mode,
     * matching the spec conversion. f64_CPROMOTE is exact; demote may
     * overflow to Inf or produce NaN, both per IEEE. */
    case OP_F32_CONVERT_I32_S:
    case OP_F32_CONVERT_I32_U:
    case OP_F32_CONVERT_I64_S:
    case OP_F32_CONVERT_I64_U: {
        wasm_val_t v;
        int is64 = (op == OP_F32_CONVERT_I64_S || op == OP_F32_CONVERT_I64_U);
        int uns = (op == OP_F32_CONVERT_I32_U || op == OP_F32_CONVERT_I64_U);
        if (is64) {
            int64_t a = 0;
            wasm_val_t w;
            tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I64, &w);
            if (tr != WASM_TRAP_OK) {
                return tr;
            }
            a = w.value.i64;
            if (uns) {
                return vm_push_f32(vm, (float)(uint64_t)a);
            }
            return vm_push_f32(vm, (float)a);
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (uns) {
            return vm_push_f32(vm, (float)(uint32_t)v.value.i32);
        }
        return vm_push_f32(vm, (float)v.value.i32);
    }

    case OP_F64_CONVERT_I32_S:
    case OP_F64_CONVERT_I32_U:
    case OP_F64_CONVERT_I64_S:
    case OP_F64_CONVERT_I64_U: {
        wasm_val_t v;
        int is64 = (op == OP_F64_CONVERT_I64_S || op == OP_F64_CONVERT_I64_U);
        int uns = (op == OP_F64_CONVERT_I32_U || op == OP_F64_CONVERT_I64_U);
        if (is64) {
            int64_t a = 0;
            wasm_val_t w;
            tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I64, &w);
            if (tr != WASM_TRAP_OK) {
                return tr;
            }
            a = w.value.i64;
            if (uns) {
                return vm_push_f64(vm, (double)(uint64_t)a);
            }
            return vm_push_f64(vm, (double)a);
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (uns) {
            return vm_push_f64(vm, (double)(uint32_t)v.value.i32);
        }
        return vm_push_f64(vm, (double)v.value.i32);
    }

    case OP_F32_DEMOTE_F64: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f32(vm, (float)v.value.f64);
    }

    case OP_F64_PROMOTE_F32: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f64(vm, (double)v.value.f32);
    }

    /* S6: reinterprets move raw bits; NaN payloads survive untouched. */
    case OP_I32_REINTERPRET_F32: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_i32(vm, vm_u2s(vm_f32_bits(v.value.f32)));
    }

    case OP_I64_REINTERPRET_F64: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_i64(vm, vm_u2s64(vm_f64_bits(v.value.f64)));
    }

    case OP_F32_REINTERPRET_I32: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f32(vm, vm_bits_f32((uint32_t)v.value.i32));
    }

    case OP_F64_REINTERPRET_I64: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f64(vm, vm_bits_f64((uint64_t)v.value.i64));
    }

    case OP_I32_WRAP_I64: {
        /* Keep the low 32 bits; bit-exact via the u32 reinterpreter. */
        wasm_val_t v;
        uint64_t w = 0u;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        memcpy(&w, &v.value.i64, sizeof(w));
        return vm_push_i32(vm, vm_u2s((uint32_t)(w & 0xFFFFFFFFu)));
    }

    case OP_I64_EXTEND_I32_S:
    case OP_I64_EXTEND_I32_U: {
        /* Widen the i32 bit pattern; sign-extend only for _S. The OR
         * form reuses the u64 reinterpreter (no implementation-defined
         * signed conversion anywhere). */
        wasm_val_t v;
        uint32_t w = 0u;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        memcpy(&w, &v.value.i32, sizeof(w));
        if (op == OP_I64_EXTEND_I32_S && (w & 0x80000000u) != 0u) {
            return vm_push_i64(vm, vm_u2s64((uint64_t)w | 0xFFFFFFFF00000000uLL));
        }
        return vm_push_i64(vm, (int64_t)w);
    }

    case OP_I32_EQZ: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_bool(vm, v.value.i32 == 0);
    }

    case OP_I32_EQ:
    case OP_I32_NE:
    case OP_I32_LT_S:
    case OP_I32_LT_U:
    case OP_I32_GT_S:
    case OP_I32_GT_U:
    case OP_I32_LE_S:
    case OP_I32_LE_U:
    case OP_I32_GE_S:
    case OP_I32_GE_U: {
        int32_t a = 0, b = 0;
        int cond = 0;
        tr = vm_pop2_i32(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        switch (op) {
        case OP_I32_EQ:   cond = (a == b); break;
        case OP_I32_NE:   cond = (a != b); break;
        case OP_I32_LT_S: cond = (a < b); break;
        case OP_I32_LT_U: cond = ((uint32_t)a < (uint32_t)b); break;
        case OP_I32_GT_S: cond = (a > b); break;
        case OP_I32_GT_U: cond = ((uint32_t)a > (uint32_t)b); break;
        case OP_I32_LE_S: cond = (a <= b); break;
        case OP_I32_LE_U: cond = ((uint32_t)a <= (uint32_t)b); break;
        case OP_I32_GE_S: cond = (a >= b); break;
        default:          cond = ((uint32_t)a >= (uint32_t)b); break; /* GE_U */
        }
        return vm_push_bool(vm, cond);
    }

    case OP_I64_EQZ: {
        wasm_val_t v;
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_bool(vm, v.value.i64 == 0);
    }

    case OP_I64_EQ:
    case OP_I64_NE:
    case OP_I64_LT_S:
    case OP_I64_LT_U:
    case OP_I64_GT_S:
    case OP_I64_GT_U:
    case OP_I64_LE_S:
    case OP_I64_LE_U:
    case OP_I64_GE_S:
    case OP_I64_GE_U: {
        int64_t a = 0, b = 0;
        int cond = 0;
        tr = vm_pop2_i64(vm, &a, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        switch (op) {
        case OP_I64_EQ:   cond = (a == b); break;
        case OP_I64_NE:   cond = (a != b); break;
        case OP_I64_LT_S: cond = (a < b); break;
        case OP_I64_LT_U: cond = ((uint64_t)a < (uint64_t)b); break;
        case OP_I64_GT_S: cond = (a > b); break;
        case OP_I64_GT_U: cond = ((uint64_t)a > (uint64_t)b); break;
        case OP_I64_LE_S: cond = (a <= b); break;
        case OP_I64_LE_U: cond = ((uint64_t)a <= (uint64_t)b); break;
        case OP_I64_GE_S: cond = (a >= b); break;
        default:          cond = ((uint64_t)a >= (uint64_t)b); break; /* GE_U */
        }
        return vm_push_bool(vm, cond);
    }

    case OP_I32_LOAD: {
        uint32_t align = 0u, offset = 0u;
        wasm_val_t vb;
        uint64_t ea64;
        int32_t v = 0;
        tr = vm_decode_u32_imm(f, &align);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        (void)align; /* hint only; correctness never depends on it */
        tr = vm_decode_u32_imm(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        ea64 = (uint64_t)(uint32_t)vb.value.i32 + (uint64_t)offset;
        if (ea64 > (uint64_t)0xFFFFFFFFu) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        if (vm->mem == NULL) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        tr = wasm_memory_load_i32(vm->mem, (uint32_t)ea64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_i32(vm, v);
    }

    case OP_I32_STORE: {
        uint32_t align = 0u, offset = 0u;
        wasm_val_t vv;
        wasm_val_t vb;
        uint64_t ea64;
        tr = vm_decode_u32_imm(f, &align);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        (void)align;
        tr = vm_decode_u32_imm(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        /* Stack order: [base, value]; value is on top. */
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vv);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
        if (tr != WASM_TRAP_OK) {
            (void)wasm_stack_push(&vm->operands, vv);
            return tr;
        }
        ea64 = (uint64_t)(uint32_t)vb.value.i32 + (uint64_t)offset;
        if (ea64 > (uint64_t)0xFFFFFFFFu) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        if (vm->mem == NULL) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        return wasm_memory_store_i32(vm->mem, (uint32_t)ea64, vv.value.i32);
    }

    case OP_F32_LOAD: {
        uint32_t align = 0u, offset = 0u;
        wasm_val_t vb;
        uint64_t ea64;
        float v = 0.0f;
        tr = vm_decode_u32_imm(f, &align);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        (void)align; /* hint only; correctness never depends on it */
        tr = vm_decode_u32_imm(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        ea64 = (uint64_t)(uint32_t)vb.value.i32 + (uint64_t)offset;
        if (ea64 > (uint64_t)0xFFFFFFFFu) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        if (vm->mem == NULL) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        tr = wasm_memory_load_f32(vm->mem, (uint32_t)ea64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f32(vm, v);
    }

    case OP_F64_LOAD: {
        uint32_t align = 0u, offset = 0u;
        wasm_val_t vb;
        uint64_t ea64;
        double v = 0.0;
        tr = vm_decode_u32_imm(f, &align);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        (void)align; /* hint only; correctness never depends on it */
        tr = vm_decode_u32_imm(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        ea64 = (uint64_t)(uint32_t)vb.value.i32 + (uint64_t)offset;
        if (ea64 > (uint64_t)0xFFFFFFFFu) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        if (vm->mem == NULL) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        tr = wasm_memory_load_f64(vm->mem, (uint32_t)ea64, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_f64(vm, v);
    }

    case OP_F32_STORE: {
        uint32_t align = 0u, offset = 0u;
        wasm_val_t vv;
        wasm_val_t vb;
        uint64_t ea64;
        tr = vm_decode_u32_imm(f, &align);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        (void)align;
        tr = vm_decode_u32_imm(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        /* Stack order: [base, value]; value is on top. */
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F32, &vv);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
        if (tr != WASM_TRAP_OK) {
            (void)wasm_stack_push(&vm->operands, vv);
            return tr;
        }
        ea64 = (uint64_t)(uint32_t)vb.value.i32 + (uint64_t)offset;
        if (ea64 > (uint64_t)0xFFFFFFFFu) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        if (vm->mem == NULL) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        return wasm_memory_store_f32(vm->mem, (uint32_t)ea64, vv.value.f32);
    }

    case OP_F64_STORE: {
        uint32_t align = 0u, offset = 0u;
        wasm_val_t vv;
        wasm_val_t vb;
        uint64_t ea64;
        tr = vm_decode_u32_imm(f, &align);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        (void)align;
        tr = vm_decode_u32_imm(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        /* Stack order: [base, value]; value is on top. */
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_F64, &vv);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
        if (tr != WASM_TRAP_OK) {
            (void)wasm_stack_push(&vm->operands, vv);
            return tr;
        }
        ea64 = (uint64_t)(uint32_t)vb.value.i32 + (uint64_t)offset;
        if (ea64 > (uint64_t)0xFFFFFFFFu) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        if (vm->mem == NULL) {
            return WASM_TRAP_MEMORY_OUT_OF_BOUNDS;
        }
        return wasm_memory_store_f64(vm->mem, (uint32_t)ea64, vv.value.f64);
    }

    case OP_I64_LOAD: {
        uint32_t offset = 0u, ea = 0u;
        int64_t v = 0;
        tr = vm_decode_memarg(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = vm_resolve_ea(vm, offset, &ea);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_memory_load_i64(vm->mem, ea, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_i64(vm, v);
    }

    case OP_I32_LOAD8_S:
    case OP_I32_LOAD8_U: {
        uint32_t offset = 0u, ea = 0u;
        uint8_t b = 0u;
        tr = vm_decode_memarg(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = vm_resolve_ea(vm, offset, &ea);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_memory_load_u8(vm->mem, ea, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_I32_LOAD8_S && (b & 0x80u) != 0u) {
            return vm_push_i32(vm, vm_u2s((uint32_t)b | 0xFFFFFF00u));
        }
        return vm_push_i32(vm, vm_u2s((uint32_t)b));
    }

    case OP_I32_LOAD16_S:
    case OP_I32_LOAD16_U: {
        uint32_t offset = 0u, ea = 0u;
        uint16_t u = 0u;
        tr = vm_decode_memarg(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = vm_resolve_ea(vm, offset, &ea);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_memory_load_u16(vm->mem, ea, &u);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_I32_LOAD16_S && (u & 0x8000u) != 0u) {
            return vm_push_i32(vm, vm_u2s((uint32_t)u | 0xFFFF0000u));
        }
        return vm_push_i32(vm, vm_u2s((uint32_t)u));
    }

    case OP_I64_LOAD8_S:
    case OP_I64_LOAD8_U: {
        uint32_t offset = 0u, ea = 0u;
        uint8_t b = 0u;
        tr = vm_decode_memarg(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = vm_resolve_ea(vm, offset, &ea);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_memory_load_u8(vm->mem, ea, &b);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_I64_LOAD8_S && (b & 0x80u) != 0u) {
            return vm_push_i64(vm, vm_u2s64((uint64_t)b | 0xFFFFFFFFFFFFFF00uLL));
        }
        return vm_push_i64(vm, (int64_t)b);
    }

    case OP_I64_LOAD16_S:
    case OP_I64_LOAD16_U: {
        uint32_t offset = 0u, ea = 0u;
        uint16_t u = 0u;
        tr = vm_decode_memarg(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = vm_resolve_ea(vm, offset, &ea);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_memory_load_u16(vm->mem, ea, &u);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_I64_LOAD16_S && (u & 0x8000u) != 0u) {
            return vm_push_i64(vm, vm_u2s64((uint64_t)u | 0xFFFFFFFFFFFF0000uLL));
        }
        return vm_push_i64(vm, (int64_t)u);
    }

    case OP_I64_LOAD32_S:
    case OP_I64_LOAD32_U: {
        uint32_t offset = 0u, ea = 0u;
        int32_t v = 0;
        uint32_t w = 0u;
        tr = vm_decode_memarg(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = vm_resolve_ea(vm, offset, &ea);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_memory_load_i32(vm->mem, ea, &v);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        memcpy(&w, &v, sizeof(w));
        if (op == OP_I64_LOAD32_S && (w & 0x80000000u) != 0u) {
            return vm_push_i64(vm, vm_u2s64((uint64_t)w | 0xFFFFFFFF00000000uLL));
        }
        return vm_push_i64(vm, (int64_t)w);
    }

    case OP_I64_STORE: {
        uint32_t offset = 0u, ea = 0u;
        wasm_val_t vv;
        wasm_val_t vb;
        tr = vm_decode_memarg(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        /* Stack order: [base, value]; value is on top. */
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I64, &vv);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
        if (tr != WASM_TRAP_OK) {
            (void)wasm_stack_push(&vm->operands, vv);
            return tr;
        }
        tr = vm_store_ea(vm, vb.value.i32, offset, &ea);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return wasm_memory_store_i64(vm->mem, ea, vv.value.i64);
    }

    case OP_I32_STORE8:
    case OP_I32_STORE16: {
        uint32_t offset = 0u, ea = 0u;
        wasm_val_t vv;
        wasm_val_t vb;
        tr = vm_decode_memarg(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vv);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
        if (tr != WASM_TRAP_OK) {
            (void)wasm_stack_push(&vm->operands, vv);
            return tr;
        }
        tr = vm_store_ea(vm, vb.value.i32, offset, &ea);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (op == OP_I32_STORE8) {
            return wasm_memory_store_u8(vm->mem, ea,
                                        (uint8_t)((uint32_t)vv.value.i32 & 0xFFu));
        }
        return wasm_memory_store_u16(vm->mem, ea,
                                     (uint16_t)((uint32_t)vv.value.i32 & 0xFFFFu));
    }

    case OP_I64_STORE8:
    case OP_I64_STORE16:
    case OP_I64_STORE32: {
        uint32_t offset = 0u, ea = 0u;
        wasm_val_t vv;
        wasm_val_t vb;
        uint64_t w = 0u;
        tr = vm_decode_memarg(f, &offset);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I64, &vv);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vb);
        if (tr != WASM_TRAP_OK) {
            (void)wasm_stack_push(&vm->operands, vv);
            return tr;
        }
        tr = vm_store_ea(vm, vb.value.i32, offset, &ea);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        memcpy(&w, &vv.value.i64, sizeof(w));
        if (op == OP_I64_STORE8) {
            return wasm_memory_store_u8(vm->mem, ea, (uint8_t)(w & 0xFFu));
        }
        if (op == OP_I64_STORE16) {
            return wasm_memory_store_u16(vm->mem, ea, (uint16_t)(w & 0xFFFFu));
        }
        {
            int32_t lo = 0;
            uint32_t uw = (uint32_t)(w & 0xFFFFFFFFu);
            memcpy(&lo, &uw, sizeof(lo));
            return wasm_memory_store_i32(vm->mem, ea, lo);
        }
    }

    case OP_MEMORY_SIZE: {
        /* Reserved 0x00 byte, then push the current page count (0 when
         * no memory is bound). Never traps on the memory itself. */
        if (f->code_ptr == NULL || f->code_end == NULL ||
            f->code_ptr >= f->code_end) {
            return WASM_TRAP_CODE_OVERRUN;
        }
        if (*f->code_ptr++ != 0x00u) {
            return WASM_TRAP_INVALID_ARGUMENT;
        }
        if (vm->mem == NULL) {
            return vm_push_i32(vm, 0);
        }
        return vm_push_i32(vm, vm_u2s(wasm_memory_pages(vm->mem)));
    }

    case OP_MEMORY_GROW: {
        /* Reserved 0x00 byte, then grow by the i32 page delta. Failure
         * (including unbound memory) pushes -1, never traps. */
        wasm_val_t vd;
        uint32_t prev = 0u;
        if (f->code_ptr == NULL || f->code_end == NULL ||
            f->code_ptr >= f->code_end) {
            return WASM_TRAP_CODE_OVERRUN;
        }
        if (*f->code_ptr++ != 0x00u) {
            return WASM_TRAP_INVALID_ARGUMENT;
        }
        tr = wasm_stack_pop_typed(&vm->operands, WASM_TYPE_I32, &vd);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        if (vm->mem == NULL) {
            return vm_push_i32(vm, -1);
        }
        tr = wasm_mem_grow(vm->mem, (uint32_t)vd.value.i32, &prev);
        if (tr == WASM_TRAP_MEMORY_GROW_FAILED) {
            return vm_push_i32(vm, -1);
        }
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
        return vm_push_i32(vm, vm_u2s(prev));
    }

    default:
        return WASM_TRAP_UNKNOWN_OPCODE;
    }
}

wasm_trap_t wasm_vm_run(wasm_vm_t *vm)
{
    wasm_frame_t *f = NULL;
    wasm_trap_t tr;
    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    for (;;) {
        tr = wasm_call_stack_top(&vm->calls, &f);
        if (tr != WASM_TRAP_OK) {
            return WASM_TRAP_OK; /* no frames: nothing to execute */
        }
        if (f->code_ptr == NULL || f->code_end == NULL || f->code_ptr >= f->code_end) {
            return WASM_TRAP_OK; /* top frame's code consumed: finished */
        }
        tr = wasm_vm_step(vm);
        if (tr != WASM_TRAP_OK) {
            return tr;
        }
    }
}

wasm_trap_t wasm_vm_define_function(wasm_vm_t *vm, uint32_t func_idx,
                                    uint32_t param_count, uint32_t local_count,
                                    uint32_t return_count,
                                    const uint8_t *code, size_t code_len)
{
    uint64_t total;
    if (vm == NULL) {
        return WASM_TRAP_NULL;
    }
    if (func_idx >= (uint32_t)WASM_MAX_FUNCTIONS) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    if (return_count > 1u) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    total = (uint64_t)param_count + (uint64_t)local_count;
    if (total > (uint64_t)WASM_MAX_LOCALS_PER_FRAME) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    if (code_len > 0u && code == NULL) {
        return WASM_TRAP_NULL;
    }
    vm->funcs[func_idx].param_count = param_count;
    vm->funcs[func_idx].local_count = local_count;
    vm->funcs[func_idx].return_count = return_count;
    vm->funcs[func_idx].code = code;
    vm->funcs[func_idx].code_len = code_len;
    vm->funcs[func_idx].valid = 1;
    vm->funcs[func_idx].is_host = 0;
    vm->funcs[func_idx].host_cb = NULL;
    {
        uint32_t k;
        for (k = 0u; k < (uint32_t)WASM_MAX_LOCALS_PER_FRAME; k++) {
            vm->funcs[func_idx].param_types[k] = 0u;
        }
    }
    vm->funcs[func_idx].return_type = 0u;
    vm->funcs[func_idx].has_sig = 0; /* counts only: no indirect sig check */
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_vm_define_host_function(wasm_vm_t *vm, uint32_t func_idx,
                                         uint32_t param_count,
                                         uint32_t return_count,
                                         wasm_host_func_t cb)
{
    if (vm == NULL || cb == NULL) {
        return WASM_TRAP_NULL;
    }
    if (func_idx >= (uint32_t)WASM_MAX_FUNCTIONS) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    if (param_count > (uint32_t)WASM_MAX_LOCALS_PER_FRAME || return_count > 1u) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    vm->funcs[func_idx].param_count = param_count;
    vm->funcs[func_idx].local_count = 0u;
    vm->funcs[func_idx].return_count = return_count;
    vm->funcs[func_idx].code = NULL;
    vm->funcs[func_idx].code_len = 0u;
    vm->funcs[func_idx].valid = 1;
    vm->funcs[func_idx].is_host = 1;
    vm->funcs[func_idx].host_cb = cb;
    {
        uint32_t k;
        for (k = 0u; k < (uint32_t)WASM_MAX_LOCALS_PER_FRAME; k++) {
            vm->funcs[func_idx].param_types[k] = 0u;
        }
    }
    vm->funcs[func_idx].return_type = 0u;
    vm->funcs[func_idx].has_sig = 0; /* counts only: no indirect sig check */
    return WASM_TRAP_OK;
}
