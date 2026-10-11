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


wasm_err_t wasm_instantiate(wasm_vm_t *vm, const wasm_module_t *mod,
                            const wasm_host_registry_t *reg,
                            uint32_t *out_nfuncs)
{
    wasm_slice_t typep, impp, funcp, codep;
    int has_type, has_imp, has_func, has_code;
    wasm_err_t rc;
    uint32_t regmap[WASM_MAX_FUNCTIONS];
    uint32_t ftype[WASM_MAX_FUNCTIONS];
    uint32_t nimports = 0u, ndefined = 0u, i;
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

    /* 1. Function imports -> host slots. */
    if (has_imp) {
        rc = wasm_bind_imports(&impp, has_type ? &typep : NULL, reg,
                               regmap, (uint32_t)WASM_MAX_FUNCTIONS, &nimports);
        if (rc != WASM_OK) {
            return rc;
        }
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
