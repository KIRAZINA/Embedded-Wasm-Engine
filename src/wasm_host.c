/*
 * wasm_host.c — Phase 6 host registry, export lookup, import binding,
 * and whole-module instantiation. C11, -Wall -Wextra -Werror clean.
 * Zero allocation; all outputs are caller-provided or VM-embedded.
 */

#include "wasm_host.h"

#include <string.h>

#include "leb128.h"

#define WASM_TAG_FCTYPE 0x60u
#define WASM_KIND_FUNC 0x00u

/* Wrap-safe: is u32 `need` readable at [p, end)? */
static int readable(const uint8_t *p, const uint8_t *end, uint64_t need)
{
    if (p == NULL || end == NULL || p > end) {
        return 0;
    }
    return need <= (uint64_t)(size_t)(end - p) ? 1 : 0;
}

wasm_trap_t wasm_host_registry_init(wasm_host_registry_t *r)
{
    uint32_t i;
    if (r == NULL) {
        return WASM_TRAP_NULL;
    }
    for (i = 0u; i < (uint32_t)WASM_MAX_HOST_FUNCS; i++) {
        r->entries[i].module[0] = '\0';
        r->entries[i].name[0] = '\0';
        r->entries[i].param_count = 0u;
        r->entries[i].return_count = 0u;
        r->entries[i].func = NULL;
        r->entries[i].used = 0;
    }
    for (i = 0u; i < (uint32_t)WASM_MAX_HOST_GLOBALS; i++) {
        r->globals[i].module[0] = '\0';
        r->globals[i].name[0] = '\0';
        r->globals[i].type = WASM_TYPE_I32;
        r->globals[i].mut = 0;
        r->globals[i].value.type = WASM_TYPE_I32;
        r->globals[i].value.value.i32 = 0;
        r->globals[i].used = 0;
    }
    return WASM_TRAP_OK;
}

wasm_trap_t wasm_host_register_func(wasm_host_registry_t *r,
                                    const char *module, const char *name,
                                    uint32_t param_count, uint32_t return_count,
                                    wasm_host_func_t func)
{
    uint32_t i;
    size_t ml, nl;
    if (r == NULL || module == NULL || name == NULL || func == NULL) {
        return WASM_TRAP_NULL;
    }
    if (param_count > (uint32_t)WASM_MAX_LOCALS_PER_FRAME || return_count > 1u) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    ml = strlen(module);
    nl = strlen(name);
    if (ml == 0u || nl == 0u ||
        ml >= (size_t)WASM_HOST_NAME_MAX || nl >= (size_t)WASM_HOST_NAME_MAX) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    for (i = 0u; i < (uint32_t)WASM_MAX_HOST_FUNCS; i++) {
        if (!r->entries[i].used) {
            memcpy(r->entries[i].module, module, ml + 1u);
            memcpy(r->entries[i].name, name, nl + 1u);
            r->entries[i].param_count = param_count;
            r->entries[i].return_count = return_count;
            r->entries[i].func = func;
            r->entries[i].used = 1;
            return WASM_TRAP_OK;
        }
    }
    return WASM_TRAP_INVALID_ARGUMENT; /* table full */
}

static int host_valtype_ok(wasm_valtype_t t)
{
    return t == WASM_TYPE_I32 || t == WASM_TYPE_I64 ||
           t == WASM_TYPE_F32 || t == WASM_TYPE_F64;
}

wasm_trap_t wasm_host_register_global(wasm_host_registry_t *r,
                                      const char *module, const char *name,
                                      wasm_valtype_t type, int mut,
                                      const wasm_val_t *value)
{
    uint32_t i;
    size_t ml, nl;
    if (r == NULL || module == NULL || name == NULL || value == NULL) {
        return WASM_TRAP_NULL;
    }
    if (!host_valtype_ok(type) || (mut != 0 && mut != 1)) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    if (value->type != type) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    ml = strlen(module);
    nl = strlen(name);
    if (ml == 0u || nl == 0u ||
        ml >= (size_t)WASM_HOST_NAME_MAX || nl >= (size_t)WASM_HOST_NAME_MAX) {
        return WASM_TRAP_INVALID_ARGUMENT;
    }
    for (i = 0u; i < (uint32_t)WASM_MAX_HOST_GLOBALS; i++) {
        if (!r->globals[i].used) {
            memcpy(r->globals[i].module, module, ml + 1u);
            memcpy(r->globals[i].name, name, nl + 1u);
            r->globals[i].type = type;
            r->globals[i].mut = mut;
            r->globals[i].value = *value;
            r->globals[i].used = 1;
            return WASM_TRAP_OK;
        }
    }
    return WASM_TRAP_INVALID_ARGUMENT; /* table full */
}

/* Registry name match against length-delimited binary bytes. */
static int name_eq(const char *reg, const uint8_t *bin, uint32_t bin_len)
{
    size_t rl = strlen(reg);
    if ((uint64_t)bin_len != (uint64_t)rl) {
        return 0;
    }
    return memcmp(reg, bin, rl) == 0 ? 1 : 0;
}

static int32_t find_host(const wasm_host_registry_t *reg,
                         const uint8_t *mod, uint32_t mod_len,
                         const uint8_t *name, uint32_t name_len)
{
    uint32_t i;
    for (i = 0u; i < (uint32_t)WASM_MAX_HOST_FUNCS; i++) {
        if (reg->entries[i].used &&
            name_eq(reg->entries[i].module, mod, mod_len) &&
            name_eq(reg->entries[i].name, name, name_len)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t find_host_global(const wasm_host_registry_t *reg,
                                const uint8_t *mod, uint32_t mod_len,
                                const uint8_t *name, uint32_t name_len)
{
    uint32_t i;
    for (i = 0u; i < (uint32_t)WASM_MAX_HOST_GLOBALS; i++) {
        if (reg->globals[i].used &&
            name_eq(reg->globals[i].module, mod, mod_len) &&
            name_eq(reg->globals[i].name, name, name_len)) {
            return (int32_t)i;
        }
    }
    return -1;
}

wasm_err_t wasm_get_export_func_index(const wasm_slice_t *export_payload,
                                      const char *name, uint32_t *out_idx)
{
    const uint8_t *cur, *end;
    uint32_t count, i;
    size_t want;
    wasm_err_t rc;

    if (export_payload == NULL || name == NULL || out_idx == NULL) {
        return WASM_ERR_NULL;
    }
    if (export_payload->len > 0u && export_payload->data == NULL) {
        return WASM_ERR_NULL;
    }
    if (export_payload->len == 0u) {
        return WASM_ERR_EXPORT_NOT_FOUND;
    }
    cur = export_payload->data;
    end = export_payload->data + export_payload->len;
    rc = leb128_decode_u32(&cur, end, &count);
    if (rc != WASM_OK) {
        return rc;
    }
    want = strlen(name);
    for (i = 0u; i < count; i++) {
        uint32_t nlen, kind, idx;
        const uint8_t *nm;
        rc = leb128_decode_u32(&cur, end, &nlen);
        if (rc != WASM_OK) {
            return rc;
        }
        if (!readable(cur, end, nlen)) {
            return WASM_ERR_TRUNCATED;
        }
        nm = cur;
        cur += nlen;
        if (!readable(cur, end, 1u)) {
            return WASM_ERR_TRUNCATED;
        }
        kind = *cur++;
        rc = leb128_decode_u32(&cur, end, &idx);
        if (rc != WASM_OK) {
            return rc;
        }
        if (kind == WASM_KIND_FUNC && (uint64_t)nlen == (uint64_t)want &&
            (want == 0u || memcmp(nm, name, want) == 0)) {
            *out_idx = idx;
            return WASM_OK;
        }
    }
    return WASM_ERR_EXPORT_NOT_FOUND;
}

/* Skip one MVP limits field: flags, min, [max]. */
static wasm_err_t skip_limits(const uint8_t **pp, const uint8_t *end)
{
    uint32_t flags, min, maxv = 0u;
    wasm_err_t rc = leb128_decode_u32(pp, end, &flags);
    if (rc != WASM_OK) {
        return rc;
    }
    if (flags > 1u) {
        return WASM_ERR_INVALID_SECTION;
    }
    rc = leb128_decode_u32(pp, end, &min);
    if (rc != WASM_OK) {
        return rc;
    }
    if (flags & 1u) {
        rc = leb128_decode_u32(pp, end, &maxv);
        if (rc != WASM_OK) {
            return rc;
        }
    }
    (void)min;
    (void)maxv;
    return WASM_OK;
}

/* Decode functype #typeidx from a type-section payload into counts. */
static wasm_err_t parse_functype(const wasm_slice_t *typep, uint32_t typeidx,
                                 uint32_t *out_pc, uint32_t *out_rc)
{
    const uint8_t *cur, *end;
    uint32_t count, t;
    wasm_err_t rc;

    cur = typep->data;
    end = typep->data + typep->len;
    rc = leb128_decode_u32(&cur, end, &count);
    if (rc != WASM_OK) {
        return rc;
    }
    if (typeidx >= count) {
        return WASM_ERR_INVALID_SECTION;
    }
    for (t = 0u; t <= typeidx; t++) {
        uint32_t pc, rcnt, k;
        uint8_t tag;
        if (!readable(cur, end, 1u)) {
            return WASM_ERR_TRUNCATED;
        }
        tag = *cur++;
        if (tag != WASM_TAG_FCTYPE) {
            return WASM_ERR_INVALID_SECTION;
        }
        rc = leb128_decode_u32(&cur, end, &pc);
        if (rc != WASM_OK) {
            return rc;
        }
        for (k = 0u; k < pc; k++) {
            uint8_t vt;
            if (!readable(cur, end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            vt = *cur++;
            if (vt != (uint8_t)WASM_TYPE_I32 && vt != (uint8_t)WASM_TYPE_I64 &&
                vt != (uint8_t)WASM_TYPE_F32 && vt != (uint8_t)WASM_TYPE_F64) {
                return WASM_ERR_INVALID_SECTION;
            }
        }
        rc = leb128_decode_u32(&cur, end, &rcnt);
        if (rc != WASM_OK) {
            return rc;
        }
        for (k = 0u; k < rcnt; k++) {
            uint8_t vt;
            if (!readable(cur, end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            vt = *cur++;
            if (vt != (uint8_t)WASM_TYPE_I32 && vt != (uint8_t)WASM_TYPE_I64 &&
                vt != (uint8_t)WASM_TYPE_F32 && vt != (uint8_t)WASM_TYPE_F64) {
                return WASM_ERR_INVALID_SECTION;
            }
        }
        if (t == typeidx) {
            *out_pc = pc;
            *out_rc = rcnt;
            return WASM_OK;
        }
    }
    return WASM_ERR_INVALID_SECTION; /* unreachable */
}

wasm_err_t wasm_bind_imports(const wasm_slice_t *import_payload,
                             const wasm_slice_t *type_payload,
                             const wasm_host_registry_t *reg,
                             uint32_t *out_reg_idx, uint32_t cap,
                             uint32_t *out_nfuncs)
{
    const uint8_t *cur, *end;
    uint32_t count, i, nfuncs = 0u;
    wasm_err_t rc;
    int check_types;

    if (import_payload == NULL || out_reg_idx == NULL || out_nfuncs == NULL) {
        return WASM_ERR_NULL;
    }
    if (import_payload->len > 0u && import_payload->data == NULL) {
        return WASM_ERR_NULL;
    }
    if (import_payload->len == 0u) {
        *out_nfuncs = 0u;
        return WASM_OK;
    }
    cur = import_payload->data;
    end = import_payload->data + import_payload->len;
    rc = leb128_decode_u32(&cur, end, &count);
    if (rc != WASM_OK) {
        return rc;
    }
    check_types = (type_payload != NULL && type_payload->len > 0u &&
                   type_payload->data != NULL) ? 1 : 0;
    for (i = 0u; i < count; i++) {
        uint32_t mlen, nlen, kind;
        const uint8_t *mb, *nb;
        rc = leb128_decode_u32(&cur, end, &mlen);
        if (rc != WASM_OK) {
            return rc;
        }
        if (!readable(cur, end, mlen)) {
            return WASM_ERR_TRUNCATED;
        }
        mb = cur;
        cur += mlen;
        rc = leb128_decode_u32(&cur, end, &nlen);
        if (rc != WASM_OK) {
            return rc;
        }
        if (!readable(cur, end, nlen)) {
            return WASM_ERR_TRUNCATED;
        }
        nb = cur;
        cur += nlen;
        if (!readable(cur, end, 1u)) {
            return WASM_ERR_TRUNCATED;
        }
        kind = *cur++;
        if (kind == WASM_KIND_FUNC) {
            uint32_t typeidx, pc = 0u, rcnt = 0u;
            int32_t slot;
            rc = leb128_decode_u32(&cur, end, &typeidx);
            if (rc != WASM_OK) {
                return rc;
            }
            if (reg == NULL) {
                return WASM_ERR_UNRESOLVED_IMPORT;
            }
            slot = find_host(reg, mb, mlen, nb, nlen);
            if (slot < 0) {
                return WASM_ERR_UNRESOLVED_IMPORT;
            }
            if (check_types) {
                rc = parse_functype(type_payload, typeidx, &pc, &rcnt);
                if (rc != WASM_OK) {
                    return rc;
                }
                if (pc != reg->entries[(uint32_t)slot].param_count ||
                    rcnt != reg->entries[(uint32_t)slot].return_count) {
                    return WASM_ERR_TYPE_MISMATCH_IMPORT;
                }
            }
            if (nfuncs >= cap) {
                return WASM_ERR_SECTION_OVERFLOW;
            }
            out_reg_idx[nfuncs++] = (uint32_t)slot;
        } else if (kind == 0x01u) {
            /* table: reftype + limits */
            if (!readable(cur, end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            cur++;
            rc = skip_limits(&cur, end);
            if (rc != WASM_OK) {
                return rc;
            }
        } else if (kind == 0x02u) {
            /* memory: limits */
            rc = skip_limits(&cur, end);
            if (rc != WASM_OK) {
                return rc;
            }
        } else if (kind == 0x03u) {
            /* global: valtype + mutability */
            if (!readable(cur, end, 2u)) {
                return WASM_ERR_TRUNCATED;
            }
            cur += 2u;
        } else {
            return WASM_ERR_INVALID_SECTION;
        }
    }
    *out_nfuncs = nfuncs;
    return WASM_OK;
}

wasm_err_t wasm_bind_global_imports(const wasm_slice_t *import_payload,
                                    const wasm_host_registry_t *reg,
                                    uint32_t *out_reg_idx, uint32_t cap,
                                    uint32_t *out_nglobals)
{
    const uint8_t *cur, *end;
    uint32_t count, i, nglobals = 0u;
    wasm_err_t rc;

    if (import_payload == NULL || out_reg_idx == NULL ||
        out_nglobals == NULL) {
        return WASM_ERR_NULL;
    }
    if (import_payload->len > 0u && import_payload->data == NULL) {
        return WASM_ERR_NULL;
    }
    if (import_payload->len == 0u) {
        *out_nglobals = 0u;
        return WASM_OK;
    }
    cur = import_payload->data;
    end = import_payload->data + import_payload->len;
    rc = leb128_decode_u32(&cur, end, &count);
    if (rc != WASM_OK) {
        return rc;
    }
    for (i = 0u; i < count; i++) {
        uint32_t mlen, nlen, kind;
        const uint8_t *mb, *nb;
        rc = leb128_decode_u32(&cur, end, &mlen);
        if (rc != WASM_OK) {
            return rc;
        }
        if (!readable(cur, end, mlen)) {
            return WASM_ERR_TRUNCATED;
        }
        mb = cur;
        cur += mlen;
        rc = leb128_decode_u32(&cur, end, &nlen);
        if (rc != WASM_OK) {
            return rc;
        }
        if (!readable(cur, end, nlen)) {
            return WASM_ERR_TRUNCATED;
        }
        nb = cur;
        cur += nlen;
        if (!readable(cur, end, 1u)) {
            return WASM_ERR_TRUNCATED;
        }
        kind = *cur++;
        if (kind == WASM_KIND_FUNC) {
            uint32_t ignored = 0u; /* typeidx */
            rc = leb128_decode_u32(&cur, end, &ignored);
            if (rc != WASM_OK) {
                return rc;
            }
            (void)ignored;
        } else if (kind == 0x01u) {
            /* table: reftype + limits */
            if (!readable(cur, end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            cur++;
            rc = skip_limits(&cur, end);
            if (rc != WASM_OK) {
                return rc;
            }
        } else if (kind == 0x02u) {
            /* memory: limits */
            rc = skip_limits(&cur, end);
            if (rc != WASM_OK) {
                return rc;
            }
        } else if (kind == 0x03u) {
            uint8_t vt, mut;
            int32_t slot;
            if (!readable(cur, end, 2u)) {
                return WASM_ERR_TRUNCATED;
            }
            vt = *cur++;
            mut = *cur++;
            if (vt != (uint8_t)WASM_TYPE_I32 && vt != (uint8_t)WASM_TYPE_I64 &&
                vt != (uint8_t)WASM_TYPE_F32 && vt != (uint8_t)WASM_TYPE_F64) {
                return WASM_ERR_INVALID_SECTION;
            }
            if (mut != 0u && mut != 1u) {
                return WASM_ERR_INVALID_SECTION;
            }
            if (reg == NULL) {
                return WASM_ERR_UNRESOLVED_IMPORT;
            }
            slot = find_host_global(reg, mb, mlen, nb, nlen);
            if (slot < 0) {
                return WASM_ERR_UNRESOLVED_IMPORT;
            }
            if ((uint8_t)reg->globals[(uint32_t)slot].type != vt ||
                (uint32_t)reg->globals[(uint32_t)slot].mut != (uint32_t)mut) {
                return WASM_ERR_TYPE_MISMATCH_IMPORT;
            }
            if (nglobals >= cap) {
                return WASM_ERR_SECTION_OVERFLOW;
            }
            out_reg_idx[nglobals++] = (uint32_t)slot;
        } else {
            return WASM_ERR_INVALID_SECTION;
        }
    }
    *out_nglobals = nglobals;
    return WASM_OK;
}

/* Optional section fetch: TRUNCATED (absent) -> empty slice, else propagate. */
static wasm_err_t opt_section(const wasm_module_t *mod, uint8_t id,
                              wasm_slice_t *out, int *present)
{
    wasm_section_header_t h;
    wasm_err_t rc = wasm_find_section(mod, id, &h);
    if (rc == WASM_ERR_TRUNCATED) {
        out->data = NULL;
        out->len = 0u;
        *present = 0;
        return WASM_OK;
    }
    if (rc != WASM_OK) {
        return rc;
    }
    out->data = h.payload.data;
    out->len = h.payload.len;
    *present = 1;
    return WASM_OK;
}

/* Decode one raw little-endian float immediate (f32.const/f64.const). */
static wasm_err_t take_raw(const uint8_t **pp, const uint8_t *end,
                           uint64_t *out, unsigned nbytes)
{
    uint64_t u = 0u;
    unsigned k;
    if (!readable(*pp, end, nbytes)) {
        return WASM_ERR_TRUNCATED;
    }
    for (k = 0u; k < nbytes; k++) {
        u |= ((uint64_t)(*pp)[k]) << (8u * k);
    }
    *pp += nbytes;
    *out = u;
    return WASM_OK;
}

static int global_valtype_ok(uint8_t vt)
{
    return vt == (uint8_t)WASM_TYPE_I32 || vt == (uint8_t)WASM_TYPE_I64 ||
           vt == (uint8_t)WASM_TYPE_F32 || vt == (uint8_t)WASM_TYPE_F64;
}

/*
 * Evaluate a constant init expression: exactly one const instruction (or a
 * global.get of an already-imported IMMUTABLE global, per MVP) followed by
 * 0x0B. Anything else is malformed (WASM_ERR_INVALID_SECTION); truncation
 * propagates the LEB/TRUNCATED code. nimports/imports describe the
 * import-global prefix of the final index space (only those are visible).
 */
static wasm_err_t eval_init_expr(const uint8_t **pp, const uint8_t *end,
                                 const wasm_global_t *imports,
                                 uint32_t nimports, wasm_val_t *out)
{
    uint8_t op;
    wasm_err_t rc;
    if (pp == NULL || *pp == NULL || end == NULL || out == NULL) {
        return WASM_ERR_NULL;
    }
    if (!readable(*pp, end, 1u)) {
        return WASM_ERR_TRUNCATED;
    }
    op = *(*pp)++;
    if (op == 0x41u) {
        int32_t v = 0;
        rc = leb128_decode_s32(pp, end, &v);
        if (rc != WASM_OK) {
            return rc;
        }
        out->type = WASM_TYPE_I32;
        out->value.i32 = v;
    } else if (op == 0x42u) {
        int64_t v = 0;
        rc = leb128_decode_s64(pp, end, &v);
        if (rc != WASM_OK) {
            return rc;
        }
        out->type = WASM_TYPE_I64;
        out->value.i64 = v;
    } else if (op == 0x43u) {
        uint64_t u = 0u;
        uint32_t b;
        rc = take_raw(pp, end, &u, 4u);
        if (rc != WASM_OK) {
            return rc;
        }
        b = (uint32_t)u;
        out->type = WASM_TYPE_F32;
        memcpy(&out->value.f32, &b, sizeof(b)); /* bit-exact */
    } else if (op == 0x44u) {
        uint64_t u = 0u;
        rc = take_raw(pp, end, &u, 8u);
        if (rc != WASM_OK) {
            return rc;
        }
        out->type = WASM_TYPE_F64;
        memcpy(&out->value.f64, &u, sizeof(u)); /* bit-exact */
    } else if (op == 0x23u) {
        uint32_t idx = 0u;
        rc = leb128_decode_u32(pp, end, &idx);
        if (rc != WASM_OK) {
            return rc;
        }
        /* Only imported IMMUTABLE globals are visible to init exprs. */
        if (imports == NULL || idx >= nimports || !imports[idx].valid ||
            imports[idx].mut != 0) {
            return WASM_ERR_INVALID_SECTION;
        }
        *out = imports[idx].value;
    } else {
        return WASM_ERR_INVALID_SECTION;
    }
    if (!readable(*pp, end, 1u) || **pp != 0x0Bu) {
        return WASM_ERR_INVALID_SECTION; /* missing terminator/trailing bytes */
    }
    (*pp)++;
    return WASM_OK;
}

/*
 * Commit one data segment's validated bounds. Visible bytes come from the
 * already-bound linear memory (NULL or short memory -> DATA_OUT_OF_BOUNDS).
 * 64-bit subtraction-form math defeats offset+len wraparound.
 */
static wasm_err_t check_data_bounds(const wasm_memory_t *mem, uint32_t offset,
                                    uint32_t len)
{
    uint64_t total, room;
    if (mem == NULL) {
        return WASM_ERR_DATA_OUT_OF_BOUNDS;
    }
    total = wasm_memory_size_bytes(mem);
    if ((uint64_t)len > total) {
        return WASM_ERR_DATA_OUT_OF_BOUNDS;
    }
    room = total - (uint64_t)len;
    if ((uint64_t)offset > room) {
        return WASM_ERR_DATA_OUT_OF_BOUNDS;
    }
    return WASM_OK;
}

/* Parse one data segment header at *pp: memidx==0, i32 offset expr, length.
 * The byte window is borrowed via *out_bytes WITHOUT advancing past it;
 * callers advance *pp by len themselves after their own checks. This lets
 * pass 1 report bounds failures (DATA_OUT_OF_BOUNDS) even when the file
 * is also too short to contain the bytes. */
static wasm_err_t parse_data_seg(const uint8_t **pp, const uint8_t *end,
                                 const wasm_vm_t *vm, uint32_t nimportglobals,
                                 uint32_t *out_offset, uint32_t *out_len,
                                 const uint8_t **out_bytes)
{
    uint32_t memidx = 0u, len = 0u;
    wasm_val_t off;
    wasm_err_t rc;
    off.type = WASM_TYPE_I32;
    off.value.i32 = 0;
    rc = leb128_decode_u32(pp, end, &memidx);
    if (rc != WASM_OK) {
        return rc;
    }
    if (memidx != 0u) {
        return WASM_ERR_INVALID_SECTION; /* MVP has exactly one memory */
    }
    rc = eval_init_expr(pp, end, vm->globals, nimportglobals, &off);
    if (rc != WASM_OK) {
        return rc;
    }
    if (off.type != WASM_TYPE_I32) {
        return WASM_ERR_INVALID_SECTION; /* offset must evaluate to i32 */
    }
    rc = leb128_decode_u32(pp, end, &len);
    if (rc != WASM_OK) {
        return rc;
    }
    *out_offset = (uint32_t)off.value.i32;
    *out_len = len;
    *out_bytes = *pp;
    return WASM_OK;
}

/*
 * Apply data segments (two passes over borrowed bytes: validate every
 * segment's bounds first, copy only after all pass — no partial writes).
 * Empty section (count 0) is a no-op, even with NULL memory.
 */
static wasm_err_t apply_data(wasm_vm_t *vm, const wasm_slice_t *datap,
                             uint32_t nimportglobals)
{
    const uint8_t *cur, *end;
    uint32_t count, i;
    wasm_err_t rc;
    cur = datap->data;
    end = datap->data + datap->len;
    rc = leb128_decode_u32(&cur, end, &count);
    if (rc != WASM_OK) {
        return rc;
    }
    if (count > (uint32_t)WASM_MAX_DATA_SEGMENTS) {
        return WASM_ERR_SECTION_OVERFLOW;
    }
    for (i = 0u; i < count; i++) {
        uint32_t offset = 0u, len = 0u;
        const uint8_t *bytes = NULL;
        rc = parse_data_seg(&cur, end, vm, nimportglobals,
                            &offset, &len, &bytes);
        if (rc != WASM_OK) {
            return rc;
        }
        rc = check_data_bounds(vm->mem, offset, len);
        if (rc != WASM_OK) {
            return rc;
        }
        if (!readable(cur, end, len)) {
            return WASM_ERR_TRUNCATED;
        }
        cur += len;
    }
    if (cur != end) {
        return WASM_ERR_INVALID_SECTION; /* trailing bytes */
    }
    /* Pass 2: re-walk (deterministic: pass 1 accepted every segment)
     * and copy in section order, so later segments overwrite earlier. */
    cur = datap->data;
    rc = leb128_decode_u32(&cur, end, &count);
    if (rc != WASM_OK) {
        return rc; /* unreachable: pass 1 accepted */
    }
    for (i = 0u; i < count; i++) {
        uint32_t offset = 0u, len = 0u;
        const uint8_t *bytes = NULL;
        rc = parse_data_seg(&cur, end, vm, nimportglobals,
                            &offset, &len, &bytes);
        if (rc != WASM_OK) {
            return rc; /* unreachable: pass 1 accepted */
        }
        if (len > 0u) {
            memcpy(&vm->mem->bytes[offset], bytes, len);
        }
        cur += len;
    }
    return WASM_OK;
}

/*
 * Run the start function ([]->[] only). Defined functions execute via
 * enter/attach/run; host imports are invoked directly. Any failure aborts
 * with WASM_ERR_START_FAILED (NULL misuse stays WASM_ERR_NULL).
 */
static wasm_err_t run_start(wasm_vm_t *vm, uint32_t funcidx, uint32_t nfuncs)
{
    const wasm_func_desc_t *d;
    wasm_trap_t tr;
    if (funcidx >= nfuncs || funcidx >= (uint32_t)WASM_MAX_FUNCTIONS) {
        return WASM_ERR_INVALID_SECTION;
    }
    d = &vm->funcs[funcidx];
    if (!d->valid) {
        return WASM_ERR_INVALID_SECTION;
    }
    if (d->param_count != 0u || d->return_count != 0u) {
        return WASM_ERR_TYPE_MISMATCH_IMPORT; /* start must be [] -> [] */
    }
    if (d->is_host) {
        wasm_val_t argv[1];
        wasm_val_t retv[1];
        argv[0].type = WASM_TYPE_I32;
        argv[0].value.i32 = 0;
        retv[0].type = WASM_TYPE_I32;
        retv[0].value.i32 = 0;
        if (d->host_cb == NULL) {
            return WASM_ERR_START_FAILED;
        }
        tr = d->host_cb(vm, argv, 0u, retv, 0u);
        return (tr == WASM_TRAP_OK) ? WASM_OK : WASM_ERR_START_FAILED;
    }
    tr = wasm_vm_enter_function(vm, funcidx, 0u, d->local_count, 0u);
    if (tr != WASM_TRAP_OK) {
        return WASM_ERR_START_FAILED;
    }
    tr = wasm_vm_attach_code(vm, d->code, d->code_len);
    if (tr != WASM_TRAP_OK) {
        wasm_frame_t *top = NULL;
        if (wasm_call_stack_top(&vm->calls, &top) == WASM_TRAP_OK && top != NULL &&
            top->func_idx == funcidx) {
            wasm_frame_t drop;
            (void)wasm_call_stack_pop(&vm->calls, &drop);
        }
        return WASM_ERR_START_FAILED;
    }
    tr = wasm_vm_run(vm);
    if (tr != WASM_TRAP_OK) {
        return WASM_ERR_START_FAILED;
    }
    tr = wasm_vm_exit_function(vm, 0u);
    return (tr == WASM_TRAP_OK) ? WASM_OK : WASM_ERR_START_FAILED;
}


wasm_err_t wasm_instantiate(wasm_vm_t *vm, const wasm_module_t *mod,
                            const wasm_host_registry_t *reg,
                            uint32_t *out_nfuncs)
{
    wasm_slice_t typep, impp, funcp, codep, globp, datap, startp;
    int has_type, has_imp, has_func, has_code, has_glob, has_data, has_start;
    wasm_err_t rc;
    uint32_t regmap[WASM_MAX_FUNCTIONS];
    uint32_t gregmap[WASM_MAX_GLOBALS];
    uint32_t ftype[WASM_MAX_FUNCTIONS];
    uint32_t nimports = 0u, ndefined = 0u, i;
    uint32_t nimportglobals = 0u;
    const uint8_t *cur, *end;

    if (vm == NULL || mod == NULL) {
        return WASM_ERR_NULL;
    }
    rc = opt_section(mod, WASM_SECT_TYPE, &typep, &has_type);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = opt_section(mod, WASM_SECT_IMPORT, &impp, &has_imp);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = opt_section(mod, WASM_SECT_FUNCTION, &funcp, &has_func);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = opt_section(mod, WASM_SECT_CODE, &codep, &has_code);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = opt_section(mod, WASM_SECT_GLOBAL, &globp, &has_glob);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = opt_section(mod, WASM_SECT_DATA, &datap, &has_data);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = opt_section(mod, WASM_SECT_START, &startp, &has_start);
    if (rc != WASM_OK) {
        return rc;
    }

    /* 1. Function imports -> host slots. */
    if (has_imp) {
        rc = wasm_bind_imports(&impp, has_type ? &typep : NULL, reg,
                               regmap, (uint32_t)WASM_MAX_FUNCTIONS, &nimports);
        if (rc != WASM_OK) {
            return rc;
        }
    }

    /* 1b. Global imports -> vm->globals prefix (imports precede defined). */
    vm->nglobals = 0u;
    if (has_imp) {
        rc = wasm_bind_global_imports(&impp, reg, gregmap,
                                      (uint32_t)WASM_MAX_GLOBALS,
                                      &nimportglobals);
        if (rc != WASM_OK) {
            return rc;
        }
        for (i = 0u; i < nimportglobals; i++) {
            const wasm_host_global_t *g = &reg->globals[gregmap[i]];
            vm->globals[i].value = g->value;
            vm->globals[i].mut = g->mut;
            vm->globals[i].valid = 1;
        }
        vm->nglobals = nimportglobals;
    }

    /* 2. Declared (WASM-code) functions: type index per function. */
    if (has_func) {
        uint32_t count;
        cur = funcp.data;
        end = funcp.data + funcp.len;
        rc = leb128_decode_u32(&cur, end, &count);
        if (rc != WASM_OK) {
            return rc;
        }
        if (count > (uint32_t)WASM_MAX_FUNCTIONS) {
            return WASM_ERR_SECTION_OVERFLOW;
        }
        for (i = 0u; i < count; i++) {
            rc = leb128_decode_u32(&cur, end, &ftype[i]);
            if (rc != WASM_OK) {
                return rc;
            }
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION; /* trailing bytes */
        }
        ndefined = count;
    } else if (has_code) {
        return WASM_ERR_INVALID_SECTION; /* code without function declarations */
    }

    /* 3. Code bodies: count must match; each yields (locals, body window). */
    if (!has_code && ndefined > 0u) {
        return WASM_ERR_INVALID_SECTION; /* declared but undefined */
    }

    if ((uint64_t)nimports + (uint64_t)ndefined > (uint64_t)WASM_MAX_FUNCTIONS) {
        return WASM_ERR_SECTION_OVERFLOW;
    }

    /* 4. Walk code entries and commit definitions. */
    if (has_code || ndefined > 0u) {
        uint32_t count = 0u;
        cur = codep.data;
        end = codep.data + codep.len;
        if (has_code) {
            rc = leb128_decode_u32(&cur, end, &count);
            if (rc != WASM_OK) {
                return rc;
            }
        }
        if (count != ndefined) {
            return WASM_ERR_INVALID_SECTION;
        }
        for (i = 0u; i < count; i++) {
            uint32_t body_size, nvec, v;
            const uint8_t *body, *body_end, *code;
            size_t code_len;
            uint64_t locals_total = 0u;
            uint32_t pc = 0u, rcnt = 0u;
            wasm_trap_t tr;
            rc = leb128_decode_u32(&cur, end, &body_size);
            if (rc != WASM_OK) {
                return rc;
            }
            if (!readable(cur, end, body_size)) {
                return WASM_ERR_TRUNCATED;
            }
            body = cur;
            body_end = cur + body_size;
            cur += body_size;
            rc = leb128_decode_u32(&body, body_end, &nvec);
            if (rc != WASM_OK) {
                return rc;
            }
            for (v = 0u; v < nvec; v++) {
                uint32_t cnt;
                uint8_t vt;
                rc = leb128_decode_u32(&body, body_end, &cnt);
                if (rc != WASM_OK) {
                    return rc;
                }
                if (!readable(body, body_end, 1u)) {
                    return WASM_ERR_TRUNCATED;
                }
                vt = *body++;
                if (vt != (uint8_t)WASM_TYPE_I32 && vt != (uint8_t)WASM_TYPE_I64 &&
                    vt != (uint8_t)WASM_TYPE_F32 && vt != (uint8_t)WASM_TYPE_F64) {
                    return WASM_ERR_INVALID_SECTION;
                }
                locals_total += (uint64_t)cnt;
                if (locals_total > (uint64_t)WASM_MAX_LOCALS_PER_FRAME) {
                    return WASM_ERR_TYPE_MISMATCH_IMPORT; /* beyond engine */
                }
            }
            code = body;
            code_len = (size_t)(body_end - body);
            if (!has_type) {
                return WASM_ERR_INVALID_SECTION; /* typeidx unresolvable */
            }
            rc = parse_functype(&typep, ftype[i], &pc, &rcnt);
            if (rc != WASM_OK) {
                return rc;
            }
            if (rcnt > 1u ||
                (uint64_t)pc + locals_total > (uint64_t)WASM_MAX_LOCALS_PER_FRAME) {
                return WASM_ERR_TYPE_MISMATCH_IMPORT; /* beyond engine */
            }
            tr = wasm_vm_define_function(vm, nimports + i, pc,
                                         (uint32_t)locals_total, rcnt,
                                         code_len > 0u ? code : NULL, code_len);
            if (tr != WASM_TRAP_OK) {
                return (tr == WASM_TRAP_NULL) ? WASM_ERR_NULL
                                              : WASM_ERR_TYPE_MISMATCH_IMPORT;
            }
        }
        if (has_code && cur != end) {
            return WASM_ERR_INVALID_SECTION; /* trailing bytes */
        }
    }

    /* 5. Commit host bindings after all validation succeeded. */
    for (i = 0u; i < nimports; i++) {
        const wasm_host_entry_t *e = &reg->entries[regmap[i]];
        wasm_trap_t tr = wasm_vm_define_host_function(vm, i, e->param_count,
                                                      e->return_count, e->func);
        if (tr != WASM_TRAP_OK) {
            return (tr == WASM_TRAP_NULL) ? WASM_ERR_NULL
                                          : WASM_ERR_TYPE_MISMATCH_IMPORT;
        }
    }

    /* 6. Defined globals: type, mutability, const-only init expr. */
    if (has_glob) {
        uint32_t count;
        cur = globp.data;
        end = globp.data + globp.len;
        rc = leb128_decode_u32(&cur, end, &count);
        if (rc != WASM_OK) {
            return rc;
        }
        if ((uint64_t)nimportglobals + (uint64_t)count >
            (uint64_t)WASM_MAX_GLOBALS) {
            return WASM_ERR_SECTION_OVERFLOW;
        }
        for (i = 0u; i < count; i++) {
            uint8_t vt, mut;
            wasm_val_t init;
            if (!readable(cur, end, 2u)) {
                return WASM_ERR_TRUNCATED;
            }
            vt = *cur++;
            mut = *cur++;
            if (!global_valtype_ok(vt) || (mut != 0u && mut != 1u)) {
                return WASM_ERR_INVALID_SECTION;
            }
            init.type = WASM_TYPE_I32;
            init.value.i32 = 0;
            rc = eval_init_expr(&cur, end, vm->globals, nimportglobals,
                                &init);
            if (rc != WASM_OK) {
                return rc;
            }
            if ((uint8_t)init.type != vt) {
                return WASM_ERR_INVALID_SECTION; /* init/type mismatch */
            }
            vm->globals[nimportglobals + i].value = init;
            vm->globals[nimportglobals + i].mut = (int)mut;
            vm->globals[nimportglobals + i].valid = 1;
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION; /* trailing bytes */
        }
        vm->nglobals = nimportglobals + count;
    }

    /* 7. Data segments into the already-bound memory (validate all, then
     * copy: no partial writes on failure). */
    if (has_data) {
        rc = apply_data(vm, &datap, nimportglobals);
        if (rc != WASM_OK) {
            return rc;
        }
    }

    /* 8. Start function ([]->[] only), last. */
    if (has_start) {
        uint32_t funcidx = 0u;
        cur = startp.data;
        end = startp.data + startp.len;
        rc = leb128_decode_u32(&cur, end, &funcidx);
        if (rc != WASM_OK) {
            return rc;
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION; /* trailing bytes */
        }
        if (funcidx < nimports) {
            const wasm_host_entry_t *e = &reg->entries[regmap[funcidx]];
            if (e->param_count != 0u || e->return_count != 0u) {
                return WASM_ERR_TYPE_MISMATCH_IMPORT;
            }
        } else if (funcidx < nimports + ndefined) {
            uint32_t pc = 0u, rcnt = 0u;
            if (!has_type) {
                return WASM_ERR_INVALID_SECTION;
            }
            rc = parse_functype(&typep, ftype[funcidx - nimports], &pc,
                                &rcnt);
            if (rc != WASM_OK) {
                return rc;
            }
            if (pc != 0u || rcnt != 0u) {
                return WASM_ERR_TYPE_MISMATCH_IMPORT;
            }
        }
        rc = run_start(vm, funcidx, nimports + ndefined);
        if (rc != WASM_OK) {
            return rc;
        }
    }

    if (out_nfuncs != NULL) {
        *out_nfuncs = nimports + ndefined;
    }
    return WASM_OK;
}

wasm_err_t wasm_get_memory_min(const wasm_module_t *mod,
                               uint32_t *out_min, int *out_present)
{
    wasm_section_header_t h;
    const uint8_t *cur, *end;
    uint32_t count, flags, min, maxv = 0u;
    wasm_err_t rc;

    if (mod == NULL || out_min == NULL || out_present == NULL) {
        return WASM_ERR_NULL;
    }
    rc = wasm_find_section(mod, (uint8_t)WASM_SECT_MEMORY, &h);
    if (rc == WASM_ERR_TRUNCATED) {
        *out_min = 0u;
        *out_present = 0;
        return WASM_OK;
    }
    if (rc != WASM_OK) {
        return rc;
    }
    if (h.payload.len == 0u) {
        return WASM_ERR_TRUNCATED;
    }
    cur = h.payload.data;
    end = h.payload.data + h.payload.len;
    rc = leb128_decode_u32(&cur, end, &count);
    if (rc != WASM_OK) {
        return rc;
    }
    if (count == 0u) {
        *out_min = 0u;
        *out_present = 0;
        return WASM_OK;
    }
    rc = leb128_decode_u32(&cur, end, &flags);
    if (rc != WASM_OK) {
        return rc;
    }
    if (flags > 1u) {
        return WASM_ERR_INVALID_SECTION;
    }
    rc = leb128_decode_u32(&cur, end, &min);
    if (rc != WASM_OK) {
        return rc;
    }
    if (flags & 1u) {
        rc = leb128_decode_u32(&cur, end, &maxv);
        if (rc != WASM_OK) {
            return rc;
        }
    }
    (void)maxv;
    *out_min = min;
    *out_present = 1;
    return WASM_OK;
}
