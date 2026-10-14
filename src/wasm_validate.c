/*
 * wasm_validate.c — Phase 5 static module validation (WebAssembly 1.0).
 *
 * Implements the spec's validation algorithm (abstract ctrl-frame +
 * value-stack machine) over every function body, plus module-level
 * structural checks. C11, -Wall -Wextra -Werror clean. No malloc.
 *
 * Error discipline (MALFORMED vs INVALID):
 * - Structure/encoding issues reuse the historic codes (TRUNCATED,
 *   LEB_*, SECTION_OVERFLOW, INVALID_SECTION, ...), chosen to match
 *   what wasm_instantiate() reports for the same bytes, so existing
 *   expectations never shift depending on which layer fires first.
 * - Typing-rule violations use the WASM_ERR_INVALID_* family, which
 *   only the body-typing machine (and friends) can produce.
 *
 * Engine-convention notes: bodies without a trailing 0x0B are accepted
 * (implicit end, matching execution); a trailing 0x0B closing an empty
 * control stack is also accepted. Block results accept all four MVP
 * valtypes (arity still at most one). Locals carry declared types.
 * Opcodes the interpreter cannot execute are INVALID_SECTION.
 */

#include "wasm_validate.h"

#include "leb128.h"

#include <string.h>

/* ---- small local utilities (mirror the host's, kept static here) ---- */

#define V_TAG_FCTYPE 0x60u
#define V_BLOCK_VOID 0x40u
#define V_UNKNOWN 0x00u /* polymorphic bottom marker (no valtype is 0) */

/* Wrap-safe readability probe. */
static int vreadable(const uint8_t *p, const uint8_t *end, uint64_t need)
{
    if (p == NULL || end == NULL || p > end) {
        return 0;
    }
    return need <= (uint64_t)(size_t)(end - p) ? 1 : 0;
}

static int vtype_ok(uint8_t vt)
{
    return vt == (uint8_t)WASM_TYPE_I32 || vt == (uint8_t)WASM_TYPE_I64 ||
           vt == (uint8_t)WASM_TYPE_F32 || vt == (uint8_t)WASM_TYPE_F64;
}

/* ---- module context (fixed arrays, borrowed slices) ---- */

typedef struct {
    wasm_slice_t types;
    int has_types;
    uint32_t ntypes;
    uint32_t ftype[WASM_MAX_FUNCTIONS]; /* per DEFINED func */
    uint32_t ndefined;
    uint32_t itype[WASM_MAX_FUNCTIONS]; /* per IMPORTED func */
    uint32_t nimports;
    uint32_t nimportglobals;
    uint8_t gtype[WASM_MAX_GLOBALS]; /* imports first, then defined */
    uint8_t gmut[WASM_MAX_GLOBALS];
    uint32_t nglobals;
} vmod_t;

/* ---- abstract machine state ---- */

typedef struct {
    uint8_t kind;   /* 0 block, 1 loop, 2 if */
    uint8_t result; /* V_BLOCK_VOID or MVP valtype byte */
    uint32_t height;
    int unreachable;
    int has_else;
} vctrl_t;

typedef struct {
    uint8_t vs[WASM_VALIDATE_STACK_MAX];
    uint32_t vtop;
    vctrl_t cs[WASM_MAX_CONTROL_STACK_DEPTH];
    uint32_t cdepth;
    int func_unreachable;
    uint8_t locals[WASM_MAX_LOCALS_PER_FRAME];
    uint32_t nlocals;
    uint8_t result; /* V_BLOCK_VOID or valtype */
    vmod_t *mc;
} validator_t;

static uint32_t vheight(const validator_t *v)
{
    if (v->cdepth == 0u) {
        return 0u;
    }
    return v->cs[v->cdepth - 1u].height;
}

static int vunreach(const validator_t *v)
{
    if (v->cdepth == 0u) {
        return v->func_unreachable;
    }
    return v->cs[v->cdepth - 1u].unreachable;
}

static wasm_err_t vpush(validator_t *v, uint8_t t)
{
    if (v->vtop >= (uint32_t)WASM_VALIDATE_STACK_MAX) {
        return WASM_ERR_SECTION_OVERFLOW; /* beyond engine capacity */
    }
    v->vs[v->vtop++] = t;
    return WASM_OK;
}

/* Pop expecting type t (0x00 expected = accept anything present).
 * Empty-at-height succeeds only in unreachable (polymorphic) mode. */
static wasm_err_t vpop_expect(validator_t *v, uint8_t t)
{
    uint32_t h = vheight(v);
    uint8_t u;
    if (v->vtop < h) {
        return WASM_ERR_INVALID_TYPE; /* defensive: below frame floor */
    }
    if (v->vtop == h) {
        return vunreach(v) ? WASM_OK : WASM_ERR_INVALID_TYPE;
    }
    u = v->vs[--v->vtop];
    if (t != V_UNKNOWN && u != V_UNKNOWN && u != t) {
        return WASM_ERR_INVALID_TYPE;
    }
    return WASM_OK;
}

/* Pop any single value (for select); fabricated Unknown when empty in
 * unreachable mode. */
static wasm_err_t vpop_any(validator_t *v, uint8_t *out)
{
    uint32_t h = vheight(v);
    if (v->vtop < h) {
        return WASM_ERR_INVALID_TYPE;
    }
    if (v->vtop == h) {
        if (!vunreach(v)) {
            return WASM_ERR_INVALID_TYPE;
        }
        *out = V_UNKNOWN;
        return WASM_OK;
    }
    *out = v->vs[--v->vtop];
    return WASM_OK;
}

static void vmark_unreachable(validator_t *v)
{
    if (v->cdepth == 0u) {
        v->func_unreachable = 1;
    } else {
        v->cs[v->cdepth - 1u].unreachable = 1;
    }
}

/* Check (without consuming) that a branch to frame f is arity-correct:
 * loop targets take nothing; block/if targets take their result type. */
static wasm_err_t vcheck_target(validator_t *v, const vctrl_t *f)
{
    uint32_t h = f->height;
    if (f->kind == 1u) { /* loop: MVP loops take no params */
        return WASM_OK;
    }
    if (f->result == V_BLOCK_VOID) {
        return WASM_OK;
    }
    if (v->vtop < h) {
        return WASM_ERR_INVALID_TYPE;
    }
    if (v->vtop == h) {
        return vunreach(v) ? WASM_OK : WASM_ERR_INVALID_TYPE;
    }
    {
        uint8_t u = v->vs[v->vtop - 1u];
        if (u != V_UNKNOWN && u != f->result) {
            return WASM_ERR_INVALID_TYPE;
        }
    }
    return WASM_OK;
}

/* ---- type-section helpers (borrowed bytes, no retention needed) ---- */

/* Fetch functype #idx: counts always exact; types stored up to caps. */
static wasm_err_t vtype_sig(vmod_t *mc, uint32_t idx, uint8_t *ptypes,
                            uint32_t pcap, uint32_t *out_npc, uint8_t *out_rt,
                            uint32_t *out_nrc)
{
    const uint8_t *cur, *end;
    uint32_t t;
    wasm_err_t rc;
    if (!mc->has_types) {
        return WASM_ERR_INVALID_SECTION;
    }
    cur = mc->types.data;
    end = mc->types.data + mc->types.len;
    rc = leb128_decode_u32(&cur, end, &mc->ntypes);
    if (rc != WASM_OK) {
        return rc;
    }
    if (idx >= mc->ntypes) {
        return WASM_ERR_INVALID_SECTION;
    }
    for (t = 0u; t <= idx; t++) {
        uint32_t pc, rcnt, k;
        uint8_t tag;
        if (!vreadable(cur, end, 1u)) {
            return WASM_ERR_TRUNCATED;
        }
        tag = *cur++;
        if (tag != V_TAG_FCTYPE) {
            return WASM_ERR_INVALID_SECTION;
        }
        rc = leb128_decode_u32(&cur, end, &pc);
        if (rc != WASM_OK) {
            return rc;
        }
        for (k = 0u; k < pc; k++) {
            uint8_t vt;
            if (!vreadable(cur, end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            vt = *cur++;
            if (!vtype_ok(vt)) {
                return WASM_ERR_INVALID_SECTION;
            }
            if (t == idx && ptypes != NULL && k < pcap) {
                ptypes[k] = vt;
            }
        }
        rc = leb128_decode_u32(&cur, end, &rcnt);
        if (rc != WASM_OK) {
            return rc;
        }
        for (k = 0u; k < rcnt; k++) {
            uint8_t vt;
            if (!vreadable(cur, end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            vt = *cur++;
            if (!vtype_ok(vt)) {
                return WASM_ERR_INVALID_SECTION;
            }
            if (t == idx && out_rt != NULL && k == 0u) {
                *out_rt = vt;
            }
        }
        if (t == idx) {
            *out_npc = pc;
            *out_nrc = rcnt;
            return WASM_OK;
        }
    }
    return WASM_ERR_INVALID_SECTION; /* unreachable */
}

/* Signature of function index f (imports first): exact types. */
/* ---- init-expression grammar (mirrors instantiate codes) ---- */

static wasm_err_t veval_init(const uint8_t **pp, const uint8_t *end,
                             const vmod_t *mc, uint32_t nimportglobals,
                             uint8_t *out_type)
{
    uint8_t op;
    wasm_err_t rc;
    if (pp == NULL || *pp == NULL || end == NULL || out_type == NULL) {
        return WASM_ERR_NULL;
    }
    if (!vreadable(*pp, end, 1u)) {
        return WASM_ERR_TRUNCATED;
    }
    op = *(*pp)++;
    if (op == 0x41u) {
        int32_t v = 0;
        rc = leb128_decode_s32(pp, end, &v);
        if (rc != WASM_OK) {
            return rc;
        }
        (void)v;
        *out_type = (uint8_t)WASM_TYPE_I32;
    } else if (op == 0x42u) {
        int64_t v = 0;
        rc = leb128_decode_s64(pp, end, &v);
        if (rc != WASM_OK) {
            return rc;
        }
        (void)v;
        *out_type = (uint8_t)WASM_TYPE_I64;
    } else if (op == 0x43u) {
        if (!vreadable(*pp, end, 4u)) {
            return WASM_ERR_TRUNCATED;
        }
        *pp += 4;
        *out_type = (uint8_t)WASM_TYPE_F32;
    } else if (op == 0x44u) {
        if (!vreadable(*pp, end, 8u)) {
            return WASM_ERR_TRUNCATED;
        }
        *pp += 8;
        *out_type = (uint8_t)WASM_TYPE_F64;
    } else if (op == 0x23u) {
        uint32_t idx = 0u;
        rc = leb128_decode_u32(pp, end, &idx);
        if (rc != WASM_OK) {
            return rc;
        }
        if (idx >= nimportglobals || mc->gmut[idx] != 0u) {
            return WASM_ERR_INVALID_SECTION; /* only imported immutables */
        }
        *out_type = mc->gtype[idx];
    } else {
        return WASM_ERR_INVALID_SECTION;
    }
    if (!vreadable(*pp, end, 1u) || **pp != 0x0Bu) {
        return WASM_ERR_INVALID_SECTION;
    }
    (*pp)++;
    return WASM_OK;
}

/* ---- body machine: block open/close ---- */

static wasm_err_t vopen_block(validator_t *v, uint8_t kind, uint8_t result,
                              uint32_t height)
{
    vctrl_t *f;
    if (v->cdepth >= (uint32_t)WASM_MAX_CONTROL_STACK_DEPTH) {
        return WASM_ERR_INVALID_BLOCK;
    }
    f = &v->cs[v->cdepth++];
    f->kind = kind;
    f->result = result;
    f->height = height;
    f->unreachable = 0;
    f->has_else = 0;
    return WASM_OK;
}

/* Validate a closing branch end/else: arity exactness in reachable mode,
 * silent truncation in unreachable mode. Does not pop the frame. */
static wasm_err_t vclose_check(validator_t *v, const vctrl_t *f)
{
    if (!f->unreachable) {
        if (f->result == V_BLOCK_VOID) {
            if (v->vtop != f->height) {
                return WASM_ERR_INVALID_TYPE;
            }
        } else {
            uint8_t u;
            if (v->vtop != f->height + 1u) {
                return WASM_ERR_INVALID_TYPE;
            }
            u = v->vs[v->vtop - 1u];
            if (u != V_UNKNOWN && u != f->result) {
                return WASM_ERR_INVALID_TYPE;
            }
        }
    }
    return WASM_OK;
}

/* ---- body machine: single function ---- */

static wasm_err_t vpop_operands(validator_t *v, const uint8_t *ts,
                                uint32_t n)
{
    uint32_t k;
    for (k = 0u; k < n; k++) {
        wasm_err_t rc = vpop_expect(v, ts[k]);
        if (rc != WASM_OK) {
            return rc;
        }
    }
    return WASM_OK;
}

/* Decode one ULEB128 immediate against the code window. */
static wasm_err_t vimm_u32(const uint8_t **pp, const uint8_t *end,
                           uint32_t *out)
{
    if (leb128_decode_u32(pp, end, out) != WASM_OK) {
        return WASM_ERR_TRUNCATED;
    }
    return WASM_OK;
}

/* Decode a load/store memarg (align + offset immediates) and check the
 * alignment against the access size: 2^align must not exceed it. */
static wasm_err_t vmemarg(const uint8_t **pp, const uint8_t *end,
                          uint32_t access)
{
    uint32_t align = 0u, offset = 0u;
    wasm_err_t rc = vimm_u32(pp, end, &align);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vimm_u32(pp, end, &offset);
    if (rc != WASM_OK) {
        return rc;
    }
    (void)offset; /* addressing is a runtime concern */
    if (align > 31u || (1u << align) > access) {
        return WASM_ERR_INVALID_ALIGN;
    }
    return WASM_OK;
}

/* Validate one function body. ptypes/npc/rtype/nrc describe it. */
static wasm_err_t vfunc_body(validator_t *v, const uint8_t *body,
                             const uint8_t *body_end, const uint8_t *ptypes,
                             uint32_t npc, uint8_t rtype, uint32_t nrc)
{
    const uint8_t *p = body;
    uint32_t k, nvec, total;
    wasm_err_t rc;
    /* Locals vector (mirror instantiate caps/codes). */
    rc = vimm_u32(&p, body_end, &nvec);
    if (rc != WASM_OK) {
        return rc;
    }
    total = npc;
    v->nlocals = 0u;
    for (k = 0u; k < npc && k < (uint32_t)WASM_MAX_LOCALS_PER_FRAME; k++) {
        v->locals[v->nlocals++] = ptypes[k];
    }
    if (npc > (uint32_t)WASM_MAX_LOCALS_PER_FRAME) {
        return WASM_ERR_TYPE_MISMATCH_IMPORT; /* beyond engine, as link does */
    }
    {
        uint32_t q;
        for (q = 0u; q < nvec; q++) {
            uint32_t cnt;
            uint8_t vt;
            rc = vimm_u32(&p, body_end, &cnt);
            if (rc != WASM_OK) {
                return rc;
            }
            if (!vreadable(p, body_end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            vt = *p++;
            if (!vtype_ok(vt)) {
                return WASM_ERR_INVALID_SECTION;
            }
            if (total + (uint64_t)cnt > (uint64_t)WASM_MAX_LOCALS_PER_FRAME) {
                return WASM_ERR_TYPE_MISMATCH_IMPORT;
            }
            {
                uint32_t j;
                for (j = 0u; j < cnt; j++) {
                    v->locals[v->nlocals++] = vt;
                }
            }
            total += cnt;
        }
    }
    v->vtop = 0u;
    v->cdepth = 0u;
    v->func_unreachable = 0;
    v->result = (nrc > 0u) ? rtype : V_BLOCK_VOID;
    /* Instruction walk. */
    while (p < body_end) {
        uint8_t op = *p++;
        switch (op) {
        case 0x00u: /* unreachable: stack becomes polymorphic */
            vmark_unreachable(v);
            break;
        case 0x01u: /* nop: no stack effect */
            break;
        case 0x02u:
        case 0x03u:
        case 0x04u: {
            uint8_t bt, kind;
            wasm_val_t cv;
            if (!vreadable(p, body_end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            bt = *p++;
            if (bt != V_BLOCK_VOID && !vtype_ok(bt)) {
                return WASM_ERR_INVALID_BLOCK;
            }
            kind = (op == 0x02u) ? 0u : ((op == 0x03u) ? 1u : 2u);
            if (op == 0x04u) {
                /* if consumes its i32 condition first. */
                cv.type = WASM_TYPE_I32;
                cv.value.i32 = 0;
                (void)cv;
                rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            rc = vopen_block(v, kind, bt, v->vtop);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        }
        case 0x05u: { /* else: close then-branch, open else-branch */
            vctrl_t *f;
            if (v->cdepth == 0u) {
                return WASM_ERR_INVALID_BLOCK;
            }
            f = &v->cs[v->cdepth - 1u];
            if (f->kind != 2u || f->has_else) {
                return WASM_ERR_INVALID_BLOCK;
            }
            rc = vclose_check(v, f);
            if (rc != WASM_OK) {
                return rc;
            }
            v->vtop = f->height;
            f->has_else = 1;
            f->unreachable = 0;
            break;
        }
        case 0x0Bu: { /* end: close a block, or the function itself */
            if (v->cdepth == 0u) {
                /* Trailing function end: nothing may follow it. */
                if (p != body_end) {
                    return WASM_ERR_INVALID_SECTION;
                }
                goto func_end;
            } else {
                vctrl_t f = v->cs[--v->cdepth];
                rc = vclose_check(v, &f);
                if (rc != WASM_OK) {
                    return rc;
                }
                v->vtop = f.height;
                if (f.result != V_BLOCK_VOID) {
                    rc = vpush(v, f.result);
                    if (rc != WASM_OK) {
                        return rc;
                    }
                }
            }
            break;
        }
        case 0x0Cu:
        case 0x0Du: {
            uint32_t label;
            vctrl_t *tgt;
            rc = vimm_u32(&p, body_end, &label);
            if (rc != WASM_OK) {
                return rc;
            }
            if (op == 0x0Du) {
                rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            if (label >= v->cdepth) {
                return WASM_ERR_INVALID_LABEL;
            }
            tgt = &v->cs[v->cdepth - 1u - label];
            rc = vcheck_target(v, tgt);
            if (rc != WASM_OK) {
                return rc;
            }
            vmark_unreachable(v);
            break;
        }
        case 0x0Eu: {
            /* br_table [count][labels][default]: validate every target. */
            uint32_t count, q;
            uint32_t labels[WASM_MAX_BR_TABLE_LABELS + 1u];
            rc = vimm_u32(&p, body_end, &count);
            if (rc != WASM_OK) {
                return rc;
            }
            if (count > (uint32_t)WASM_MAX_BR_TABLE_LABELS) {
                return WASM_ERR_SECTION_OVERFLOW;
            }
            if ((uint64_t)(size_t)(body_end - p) < (uint64_t)count + 1u) {
                return WASM_ERR_TRUNCATED;
            }
            for (q = 0u; q < count + 1u; q++) {
                uint32_t lab = 0u;
                if (leb128_decode_u32(&p, body_end, &lab) != WASM_OK) {
                    return WASM_ERR_TRUNCATED;
                }
                labels[q] = lab;
            }
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            for (q = 0u; q < count + 1u; q++) {
                vctrl_t *tgt;
                if (labels[q] >= v->cdepth) {
                    return WASM_ERR_INVALID_LABEL;
                }
                tgt = &v->cs[v->cdepth - 1u - labels[q]];
                rc = vcheck_target(v, tgt);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            vmark_unreachable(v);
            break;
        }
        case 0x0Fu: { /* return: function arity, then polymorphic */
            if (v->result == V_BLOCK_VOID) {
                if (v->vtop != vheight(v) && !vunreach(v)) {
                    /* Leftover values die with the frame; arity is empty.
                     * Truncateable later: just mark unreachable. */
                }
            } else {
                rc = vpop_expect(v, v->result);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            vmark_unreachable(v);
            break;
        }
        case 0x10u: {
            uint32_t idx, npc2 = 0u, nrc2 = 0u;
            uint8_t pt[WASM_MAX_LOCALS_PER_FRAME];
            uint8_t rt = V_BLOCK_VOID;
            uint32_t q;
            rc = vimm_u32(&p, body_end, &idx);
            if (rc != WASM_OK) {
                return rc;
            }
            if (idx >= v->mc->nimports + v->mc->ndefined) {
                return WASM_ERR_INVALID_TYPE; /* unknown function */
            }
            {
                uint32_t ti = (idx < v->mc->nimports)
                                  ? v->mc->itype[idx]
                                  : v->mc->ftype[idx - v->mc->nimports];
                rc = vtype_sig(v->mc, ti, pt,
                               (uint32_t)WASM_MAX_LOCALS_PER_FRAME, &npc2,
                               &rt, &nrc2);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            for (q = npc2; q > 0u; q--) {
                rc = vpop_expect(v, pt[q - 1u]);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            if (nrc2 > 1u) {
                return WASM_ERR_INVALID_TYPE; /* beyond engine, defensive */
            }
            if (nrc2 == 1u) {
                rc = vpush(v, rt);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            break;
        }
        case 0x11u: {
            uint32_t typeidx, npc2 = 0u, nrc2 = 0u;
            uint8_t pt[WASM_MAX_LOCALS_PER_FRAME];
            uint8_t rt = V_BLOCK_VOID;
            uint32_t q;
            rc = vimm_u32(&p, body_end, &typeidx);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            if (!v->mc->has_types || typeidx >= v->mc->ntypes) {
                return WASM_ERR_INVALID_TYPE; /* dangling type index */
            }
            rc = vtype_sig(v->mc, typeidx, pt,
                           (uint32_t)WASM_MAX_LOCALS_PER_FRAME, &npc2, &rt,
                           &nrc2);
            if (rc != WASM_OK) {
                return WASM_ERR_INVALID_TYPE;
            }
            for (q = npc2; q > 0u; q--) {
                rc = vpop_expect(v, pt[q - 1u]);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            if (nrc2 > 1u) {
                return WASM_ERR_INVALID_TYPE;
            }
            if (nrc2 == 1u) {
                rc = vpush(v, rt);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            break;
        }
        case 0x1Au: {
            /* MVP drop: discard one value. vpop_any fabricates Unknown
             * when empty in unreachable (polymorphic) mode, so drop
             * after unreachable is valid; reachable emptiness rejects. */
            uint8_t dt = V_UNKNOWN;
            rc = vpop_any(v, &dt);
            if (rc != WASM_OK) {
                return rc;
            }
            (void)dt;
            break;
        }
        case 0x1Bu: {
            /* MVP untyped select: cond i32 + two equal-typed operands. */
            uint8_t t1 = V_UNKNOWN, t2 = V_UNKNOWN;
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpop_any(v, &t1);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpop_any(v, &t2);
            if (rc != WASM_OK) {
                return rc;
            }
            if (t1 != V_UNKNOWN && t2 != V_UNKNOWN && t1 != t2) {
                return WASM_ERR_INVALID_TYPE;
            }
            rc = vpush(v, (t1 != V_UNKNOWN) ? t1 : t2);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        }
        case 0x20u:
        case 0x21u:
        case 0x22u: {
            uint32_t idx;
            rc = vimm_u32(&p, body_end, &idx);
            if (rc != WASM_OK) {
                return rc;
            }
            if (idx >= v->nlocals) {
                return WASM_ERR_INVALID_LOCAL;
            }
            if (op == 0x20u) {
                rc = vpush(v, v->locals[idx]);
                if (rc != WASM_OK) {
                    return rc;
                }
            } else if (op == 0x21u) {
                rc = vpop_expect(v, v->locals[idx]);
                if (rc != WASM_OK) {
                    return rc;
                }
            } else {
                rc = vpop_expect(v, v->locals[idx]);
                if (rc != WASM_OK) {
                    return rc;
                }
                rc = vpush(v, v->locals[idx]);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            break;
        }
        case 0x23u:
        case 0x24u: {
            uint32_t idx;
            rc = vimm_u32(&p, body_end, &idx);
            if (rc != WASM_OK) {
                return rc;
            }
            if (idx >= v->mc->nglobals) {
                return WASM_ERR_INVALID_GLOBAL;
            }
            if (op == 0x23u) {
                rc = vpush(v, v->mc->gtype[idx]);
                if (rc != WASM_OK) {
                    return rc;
                }
            } else {
                if (v->mc->gmut[idx] == 0) {
                    return WASM_ERR_INVALID_GLOBAL;
                }
                rc = vpop_expect(v, v->mc->gtype[idx]);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            break;
        }
        case 0x28u:
        case 0x36u: {
            /* i32.load / i32.store: memarg align/offset, 4-byte access. */
            uint32_t align = 0u, offset = 0u;
            rc = vimm_u32(&p, body_end, &align);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vimm_u32(&p, body_end, &offset);
            if (rc != WASM_OK) {
                return rc;
            }
            (void)offset; /* addressing is a runtime concern */
            if (align > 31u || (1u << align) > 4u) {
                return WASM_ERR_INVALID_ALIGN;
            }
            if (op == 0x28u) {
                rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
                if (rc != WASM_OK) {
                    return rc;
                }
                rc = vpush(v, (uint8_t)WASM_TYPE_I32);
                if (rc != WASM_OK) {
                    return rc;
                }
            } else {
                rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
                if (rc != WASM_OK) {
                    return rc;
                }
                rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            break;
        }
        case 0x2Au:
        case 0x2Bu:
        case 0x38u:
        case 0x39u: {
            /* f32/f64 load/store: memarg align/offset, 4/8-byte access. */
            uint32_t access = (op == 0x2Bu || op == 0x39u) ? 8u : 4u;
            uint8_t vt = (op == 0x2Au || op == 0x38u)
                             ? (uint8_t)WASM_TYPE_F32
                             : (uint8_t)WASM_TYPE_F64;
            rc = vmemarg(&p, body_end, access);
            if (rc != WASM_OK) {
                return rc;
            }
            if (op == 0x2Au || op == 0x2Bu) {
                rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
                if (rc != WASM_OK) {
                    return rc;
                }
                rc = vpush(v, vt);
            } else {
                rc = vpop_expect(v, vt);
                if (rc != WASM_OK) {
                    return rc;
                }
                rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            }
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        }
        case 0x29u:
        case 0x2Cu:
        case 0x2Du:
        case 0x2Eu:
        case 0x2Fu:
        case 0x30u:
        case 0x31u:
        case 0x32u:
        case 0x33u:
        case 0x34u:
        case 0x35u: {
            /* i64 and sub-word loads: memarg align/offset, then
             * [i32] -> [i64 or i32]. Access sizes: 8 / 1 / 2 / 4. */
            uint32_t access;
            uint8_t vt;
            if (op == 0x29u) {
                access = 8u;
                vt = (uint8_t)WASM_TYPE_I64;
            } else if (op == 0x2Cu || op == 0x2Du || op == 0x30u ||
                       op == 0x31u) {
                access = 1u;
                vt = (op == 0x2Cu || op == 0x2Du)
                         ? (uint8_t)WASM_TYPE_I32
                         : (uint8_t)WASM_TYPE_I64;
            } else if (op == 0x2Eu || op == 0x2Fu || op == 0x32u ||
                       op == 0x33u) {
                access = 2u;
                vt = (op == 0x2Eu || op == 0x2Fu)
                         ? (uint8_t)WASM_TYPE_I32
                         : (uint8_t)WASM_TYPE_I64;
            } else {
                access = 4u;
                vt = (uint8_t)WASM_TYPE_I64;
            }
            rc = vmemarg(&p, body_end, access);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, vt);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        }
        case 0x37u:
        case 0x3Au:
        case 0x3Bu:
        case 0x3Cu:
        case 0x3Du:
        case 0x3Eu: {
            /* i64 and sub-word stores: [i32 base, value] -> [].
             * Value type is i64 except 0x3A/0x3B (i32). */
            uint32_t access;
            uint8_t vt;
            if (op == 0x37u) {
                access = 8u;
                vt = (uint8_t)WASM_TYPE_I64;
            } else if (op == 0x3Au) {
                access = 1u;
                vt = (uint8_t)WASM_TYPE_I32;
            } else if (op == 0x3Bu) {
                access = 2u;
                vt = (uint8_t)WASM_TYPE_I32;
            } else if (op == 0x3Cu) {
                access = 1u;
                vt = (uint8_t)WASM_TYPE_I64;
            } else if (op == 0x3Du) {
                access = 2u;
                vt = (uint8_t)WASM_TYPE_I64;
            } else {
                access = 4u;
                vt = (uint8_t)WASM_TYPE_I64;
            }
            rc = vmemarg(&p, body_end, access);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpop_expect(v, vt);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        }
        case 0x3Fu:
        case 0x40u: {
            /* memory.size / memory.grow: one reserved 0x00 byte.
             * size is [] -> [i32]; grow is [i32] -> [i32]. */
            uint8_t flag;
            if (!vreadable(p, body_end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            flag = *p++;
            if (flag != 0x00u) {
                return WASM_ERR_INVALID_SECTION;
            }
            if (op == 0x40u) {
                rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        }
        case 0x41u:
        case 0x42u:
        case 0x43u:
        case 0x44u: {
            /* Const immediates: skipped exactly (truncation is TRUNCATED),
             * then the value type is pushed. */
            if (op == 0x41u) {
                int32_t w = 0;
                if (leb128_decode_s32(&p, body_end, &w) != WASM_OK) {
                    return WASM_ERR_TRUNCATED;
                }
                rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            } else if (op == 0x42u) {
                int64_t w = 0;
                if (leb128_decode_s64(&p, body_end, &w) != WASM_OK) {
                    return WASM_ERR_TRUNCATED;
                }
                rc = vpush(v, (uint8_t)WASM_TYPE_I64);
            } else if (op == 0x43u) {
                if (!vreadable(p, body_end, 4u)) {
                    return WASM_ERR_TRUNCATED;
                }
                p += 4;
                rc = vpush(v, (uint8_t)WASM_TYPE_F32);
            } else {
                if (!vreadable(p, body_end, 8u)) {
                    return WASM_ERR_TRUNCATED;
                }
                p += 8;
                rc = vpush(v, (uint8_t)WASM_TYPE_F64);
            }
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        }
        case 0x45u: /* i32.eqz: i32 -> i32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x46u: case 0x47u: case 0x48u: case 0x49u: case 0x4Au:
        case 0x4Bu: case 0x4Cu: case 0x4Du: case 0x4Eu: case 0x4Fu:
            /* i32 comparisons: (i32,i32) -> i32 */
            rc = vpop_operands(v, (const uint8_t[]){ (uint8_t)WASM_TYPE_I32,
                                                     (uint8_t)WASM_TYPE_I32 },
                               2u);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x50u: /* i64.eqz: i64 -> i32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x51u: case 0x52u: case 0x53u: case 0x54u: case 0x55u:
        case 0x56u: case 0x57u: case 0x58u: case 0x59u: case 0x5Au:
            /* i64 comparisons: (i64,i64) -> i32 */
            rc = vpop_operands(v, (const uint8_t[]){ (uint8_t)WASM_TYPE_I64,
                                                     (uint8_t)WASM_TYPE_I64 },
                               2u);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x5Bu: case 0x5Cu: case 0x5Du: case 0x5Eu: case 0x5Fu:
        case 0x60u:
            /* f32 comparisons: (f32,f32) -> i32 */
            rc = vpop_operands(v, (const uint8_t[]){ (uint8_t)WASM_TYPE_F32,
                                                     (uint8_t)WASM_TYPE_F32 },
                               2u);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x61u: case 0x62u: case 0x63u: case 0x64u: case 0x65u:
        case 0x66u:
            /* f64 comparisons: (f64,f64) -> i32 */
            rc = vpop_operands(v, (const uint8_t[]){ (uint8_t)WASM_TYPE_F64,
                                                     (uint8_t)WASM_TYPE_F64 },
                               2u);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x67u: case 0x68u: case 0x69u:
            /* i32 bit-count unaries: i32 -> i32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x6Au: case 0x6Bu: case 0x6Cu: case 0x6Du: case 0x6Eu:
        case 0x6Fu: case 0x70u: case 0x71u: case 0x72u: case 0x73u:
        case 0x74u: case 0x75u: case 0x76u: case 0x77u: case 0x78u:
            /* i32 ALU (incl. rotates): (i32,i32) -> i32 */
            rc = vpop_operands(v, (const uint8_t[]){ (uint8_t)WASM_TYPE_I32,
                                                     (uint8_t)WASM_TYPE_I32 },
                               2u);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x79u: case 0x7Au: case 0x7Bu:
            /* i64 bit-count unaries: i64 -> i64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x7Cu: case 0x7Du: case 0x7Eu: case 0x7Fu: case 0x80u:
        case 0x81u: case 0x82u: case 0x83u: case 0x84u: case 0x85u:
        case 0x86u: case 0x87u: case 0x88u: case 0x89u: case 0x8Au:
            /* i64 ALU (incl. rotates): (i64,i64) -> i64 */
            rc = vpop_operands(v, (const uint8_t[]){ (uint8_t)WASM_TYPE_I64,
                                                     (uint8_t)WASM_TYPE_I64 },
                               2u);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x8Bu: case 0x8Cu: case 0x8Du: case 0x8Eu: case 0x8Fu:
        case 0x90u: case 0x91u:
            /* f32 unaries: f32 -> f32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x92u: case 0x93u: case 0x94u: case 0x95u: case 0x96u:
        case 0x97u: case 0x98u:
            /* f32 binaries: (f32,f32) -> f32 */
            rc = vpop_operands(v, (const uint8_t[]){ (uint8_t)WASM_TYPE_F32,
                                                     (uint8_t)WASM_TYPE_F32 },
                               2u);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0x99u: case 0x9Au: case 0x9Bu: case 0x9Cu: case 0x9Du:
        case 0x9Eu: case 0x9Fu:
            /* f64 unaries: f64 -> f64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xA0u: case 0xA1u: case 0xA2u: case 0xA3u: case 0xA4u:
        case 0xA5u: case 0xA6u:
            /* f64 binaries: (f64,f64) -> f64 */
            rc = vpop_operands(v, (const uint8_t[]){ (uint8_t)WASM_TYPE_F64,
                                                     (uint8_t)WASM_TYPE_F64 },
                               2u);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xA7u:
            /* i32.wrap_i64: i64 -> i32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xA8u:
            /* i32.trunc_f32_s: f32 -> i32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xA9u:
            /* i32.trunc_f32_u: f32 -> i32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xAAu:
            /* i32.trunc_f64_s: f64 -> i32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xABu:
            /* i32.trunc_f64_u: f64 -> i32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xACu:
        case 0xADu:
            /* i64.extend_i32_s/u: i32 -> i64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xAEu:
            /* i64.trunc_f32_s: f32 -> i64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xAFu:
            /* i64.trunc_f32_u: f32 -> i64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xB0u:
            /* i64.trunc_f64_s: f64 -> i64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xB1u:
            /* i64.trunc_f64_u: f64 -> i64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xB2u:
        case 0xB3u:
            /* f32.convert_i32_*: i32 -> f32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xB4u:
        case 0xB5u:
            /* f32.convert_i64_*: i64 -> f32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xB6u:
            /* f32.demote_f64: f64 -> f32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xB7u:
        case 0xB8u:
            /* f64.convert_i32_*: i32 -> f64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xB9u:
        case 0xBAu:
            /* f64.convert_i64_*: i64 -> f64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xBBu:
            /* f64.promote_f32: f32 -> f64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xBCu:
            /* i32.reinterpret_f32: f32 -> i32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xBDu:
            /* i64.reinterpret_f64: f64 -> i64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xBEu:
            /* f32.reinterpret_i32: i32 -> f32 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I32);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F32);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        case 0xBFu:
            /* f64.reinterpret_i64: i64 -> f64 */
            rc = vpop_expect(v, (uint8_t)WASM_TYPE_I64);
            if (rc != WASM_OK) {
                return rc;
            }
            rc = vpush(v, (uint8_t)WASM_TYPE_F64);
            if (rc != WASM_OK) {
                return rc;
            }
            break;
        default:
            /* Bytes the interpreter cannot execute (nop, drop, i64
             * comparisons, sub-word/float memory ops, saturating
             * conversions, ...) are undecodable into engine semantics. */
            return WASM_ERR_INVALID_SECTION;
        }
    }
func_end:
    /* Implicit ends for still-open blocks (engine convention), then the
     * function-result check. */
    while (v->cdepth > 0u) {
        vctrl_t f = v->cs[--v->cdepth];
        rc = vclose_check(v, &f);
        if (rc != WASM_OK) {
            return rc;
        }
        v->vtop = f.height;
        if (f.result != V_BLOCK_VOID) {
            rc = vpush(v, f.result);
            if (rc != WASM_OK) {
                return rc;
            }
        }
    }
    if (v->result == V_BLOCK_VOID) {
        if (v->vtop != 0u && !v->func_unreachable) {
            return WASM_ERR_INVALID_TYPE;
        }
    } else {
        if (!v->func_unreachable) {
            uint8_t u;
            if (v->vtop != 1u) {
                return WASM_ERR_INVALID_TYPE;
            }
            u = v->vs[0];
            if (u != V_UNKNOWN && u != v->result) {
                return WASM_ERR_INVALID_TYPE;
            }
        }
    }
    return WASM_OK;
}

/* ---- module-level validation ---- */

/* Optional section fetch: absent -> empty slice. */
static wasm_err_t vopt_section(const wasm_module_t *mod, uint8_t id,
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

wasm_err_t wasm_validate_module(const wasm_module_t *mod)
{
    wasm_slice_t typep, impp, funcp, codep, globp, datap, startp;
    wasm_slice_t tablep, elemp, memp, expp;
    int has_type, has_imp, has_func, has_code, has_glob, has_data, has_start;
    int has_table, has_elem, has_mem, has_exp;
    vmod_t mc;
    validator_t v;
    wasm_err_t rc;
    uint32_t i;
    const uint8_t *cur, *end;

    if (mod == NULL) {
        return WASM_ERR_NULL;
    }
    memset(&mc, 0, sizeof(mc));
    memset(&v, 0, sizeof(v));
    v.mc = &mc;

    rc = vopt_section(mod, WASM_SECT_TYPE, &typep, &has_type);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vopt_section(mod, WASM_SECT_IMPORT, &impp, &has_imp);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vopt_section(mod, WASM_SECT_FUNCTION, &funcp, &has_func);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vopt_section(mod, WASM_SECT_CODE, &codep, &has_code);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vopt_section(mod, WASM_SECT_GLOBAL, &globp, &has_glob);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vopt_section(mod, WASM_SECT_DATA, &datap, &has_data);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vopt_section(mod, WASM_SECT_START, &startp, &has_start);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vopt_section(mod, WASM_SECT_TABLE, &tablep, &has_table);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vopt_section(mod, WASM_SECT_ELEMENT, &elemp, &has_elem);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vopt_section(mod, WASM_SECT_MEMORY, &memp, &has_mem);
    if (rc != WASM_OK) {
        return rc;
    }
    rc = vopt_section(mod, WASM_SECT_EXPORT, &expp, &has_exp);
    if (rc != WASM_OK) {
        return rc;
    }

    /* Types: full single-pass validation. */
    if (has_type) {
        uint32_t count, t;
        cur = typep.data;
        end = typep.data + typep.len;
        rc = leb128_decode_u32(&cur, end, &count);
        if (rc != WASM_OK) {
            return rc;
        }
        for (t = 0u; t < count; t++) {
            uint32_t pc, rcnt, k;
            uint8_t tag;
            if (!vreadable(cur, end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            tag = *cur++;
            if (tag != V_TAG_FCTYPE) {
                return WASM_ERR_INVALID_SECTION;
            }
            rc = leb128_decode_u32(&cur, end, &pc);
            if (rc != WASM_OK) {
                return rc;
            }
            for (k = 0u; k < pc; k++) {
                uint8_t vt;
                if (!vreadable(cur, end, 1u)) {
                    return WASM_ERR_TRUNCATED;
                }
                vt = *cur++;
                if (!vtype_ok(vt)) {
                    return WASM_ERR_INVALID_SECTION;
                }
            }
            rc = leb128_decode_u32(&cur, end, &rcnt);
            if (rc != WASM_OK) {
                return rc;
            }
            for (k = 0u; k < rcnt; k++) {
                uint8_t vt;
                if (!vreadable(cur, end, 1u)) {
                    return WASM_ERR_TRUNCATED;
                }
                vt = *cur++;
                if (!vtype_ok(vt)) {
                    return WASM_ERR_INVALID_SECTION;
                }
            }
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION;
        }
        mc.types = typep;
        mc.has_types = 1;
        /* ntypes recomputed lazily by vtype_sig; store provisional count. */
        mc.ntypes = count;
    }

    /* Imports: structure + per-kind descriptors. */
    if (has_imp) {
        uint32_t count;
        cur = impp.data;
        end = impp.data + impp.len;
        rc = leb128_decode_u32(&cur, end, &count);
        if (rc != WASM_OK) {
            return rc;
        }
        for (i = 0u; i < count; i++) {
            uint32_t mlen, nlen, kind;
            rc = leb128_decode_u32(&cur, end, &mlen);
            if (rc != WASM_OK) {
                return rc;
            }
            if (!vreadable(cur, end, mlen)) {
                return WASM_ERR_TRUNCATED;
            }
            cur += mlen;
            rc = leb128_decode_u32(&cur, end, &nlen);
            if (rc != WASM_OK) {
                return rc;
            }
            if (!vreadable(cur, end, nlen)) {
                return WASM_ERR_TRUNCATED;
            }
            cur += nlen;
            if (!vreadable(cur, end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            kind = *cur++;
            if (kind == 0x00u) {
                uint32_t ti;
                rc = leb128_decode_u32(&cur, end, &ti);
                if (rc != WASM_OK) {
                    return rc;
                }
                if (mc.nimports >= (uint32_t)WASM_MAX_FUNCTIONS) {
                    return WASM_ERR_SECTION_OVERFLOW;
                }
                mc.itype[mc.nimports++] = ti;
            } else if (kind == 0x01u) {
                return WASM_ERR_IMPORTED_TABLES_UNSUPPORTED;
            } else if (kind == 0x02u) {
                uint32_t flags, min, maxv = 0u;
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
                    if (maxv < min) {
                        return WASM_ERR_INVALID_SECTION;
                    }
                }
            } else if (kind == 0x03u) {
                uint8_t vt, mut;
                if (!vreadable(cur, end, 2u)) {
                    return WASM_ERR_TRUNCATED;
                }
                vt = *cur++;
                mut = *cur++;
                if (!vtype_ok(vt) || (mut != 0u && mut != 1u)) {
                    return WASM_ERR_INVALID_SECTION;
                }
                if (mc.nglobals >= (uint32_t)WASM_MAX_GLOBALS) {
                    return WASM_ERR_SECTION_OVERFLOW;
                }
                mc.gtype[mc.nglobals] = vt;
                mc.gmut[mc.nglobals] = mut;
                mc.nglobals++;
                mc.nimportglobals++;
            } else {
                return WASM_ERR_INVALID_SECTION;
            }
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION;
        }
    }

    /* Functions: typeidx range (codes mirror linking). */
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
            uint32_t ti;
            rc = leb128_decode_u32(&cur, end, &ti);
            if (rc != WASM_OK) {
                return rc;
            }
            if (!has_type) {
                return WASM_ERR_INVALID_SECTION;
            }
            {
                uint32_t pc = 0u, rcnt = 0u;
                rc = vtype_sig(&mc, ti, NULL, 0u, &pc, NULL, &rcnt);
                if (rc != WASM_OK) {
                    return rc;
                }
                (void)pc;
                (void)rcnt;
            }
            mc.ftype[mc.ndefined++] = ti;
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION;
        }
    }
    if ((uint64_t)mc.nimports + (uint64_t)mc.ndefined >
        (uint64_t)WASM_MAX_FUNCTIONS) {
        return WASM_ERR_SECTION_OVERFLOW;
    }

    /* Table: exactly one funcref table, sane limits. */
    if (has_table) {
        uint32_t count, flags, min, maxv = 0u;
        uint8_t elemtype;
        cur = tablep.data;
        end = tablep.data + tablep.len;
        rc = leb128_decode_u32(&cur, end, &count);
        if (rc != WASM_OK) {
            return rc;
        }
        if (count != 1u) {
            return WASM_ERR_INVALID_SECTION;
        }
        if (!vreadable(cur, end, 1u)) {
            return WASM_ERR_TRUNCATED;
        }
        elemtype = *cur++;
        if (elemtype != 0x70u) {
            return WASM_ERR_INVALID_SECTION;
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
        if (min > (uint32_t)WASM_MAX_TABLE_SIZE) {
            return WASM_ERR_SECTION_OVERFLOW;
        }
        if (flags & 1u) {
            rc = leb128_decode_u32(&cur, end, &maxv);
            if (rc != WASM_OK) {
                return rc;
            }
            if (maxv < min) {
                return WASM_ERR_INVALID_SECTION;
            }
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION;
        }
    }

    /* Memory: at most one, sane limits. */
    if (has_mem) {
        uint32_t count, flags, min, maxv = 0u;
        cur = memp.data;
        end = memp.data + memp.len;
        rc = leb128_decode_u32(&cur, end, &count);
        if (rc != WASM_OK) {
            return rc;
        }
        if (count > 1u) {
            return WASM_ERR_INVALID_SECTION;
        }
        for (i = 0u; i < count; i++) {
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
                if (maxv < min) {
                    return WASM_ERR_INVALID_SECTION;
                }
            }
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION;
        }
    }

    /* Globals: grammar + type agreement (mirror linking codes). */
    if (has_glob) {
        uint32_t count;
        cur = globp.data;
        end = globp.data + globp.len;
        rc = leb128_decode_u32(&cur, end, &count);
        if (rc != WASM_OK) {
            return rc;
        }
        if ((uint64_t)mc.nglobals + (uint64_t)count >
            (uint64_t)WASM_MAX_GLOBALS) {
            return WASM_ERR_SECTION_OVERFLOW;
        }
        /* Real walk below (kept separate for clarity). */
        {
            const uint8_t *gcur = globp.data;
            const uint8_t *gend = globp.data + globp.len;
            uint32_t gcount, g;
            rc = leb128_decode_u32(&gcur, gend, &gcount);
            if (rc != WASM_OK) {
                return rc;
            }
            for (g = 0u; g < gcount; g++) {
                uint8_t vt, mut, it;
                uint32_t nimp = 0u, q;
                /* Count imports already stored: recompute prefix length. */
                for (q = 0u; q < mc.nglobals && q < g; q++) {
                    (void)q;
                }
                nimp = mc.nglobals - g; /* imports = total - defined so far */
                if (!vreadable(gcur, gend, 2u)) {
                    return WASM_ERR_TRUNCATED;
                }
                vt = *gcur++;
                mut = *gcur++;
                if (!vtype_ok(vt) || (mut != 0u && mut != 1u)) {
                    return WASM_ERR_INVALID_SECTION;
                }
                rc = veval_init(&gcur, gend, &mc, nimp, &it);
                if (rc != WASM_OK) {
                    return rc;
                }
                if (it != vt) {
                    return WASM_ERR_INVALID_SECTION;
                }
                mc.gtype[mc.nglobals] = vt;
                mc.gmut[mc.nglobals] = mut;
                mc.nglobals++;
            }
            if (gcur != gend) {
                return WASM_ERR_INVALID_SECTION;
            }
        }
    }

    /* Exports: structural walk only (index ranges stay link-time). */
    if (has_exp) {
        uint32_t count;
        cur = expp.data;
        end = expp.data + expp.len;
        rc = leb128_decode_u32(&cur, end, &count);
        if (rc != WASM_OK) {
            return rc;
        }
        for (i = 0u; i < count; i++) {
            uint32_t nlen, kind, idx;
            rc = leb128_decode_u32(&cur, end, &nlen);
            if (rc != WASM_OK) {
                return rc;
            }
            if (!vreadable(cur, end, nlen)) {
                return WASM_ERR_TRUNCATED;
            }
            cur += nlen;
            if (!vreadable(cur, end, 1u)) {
                return WASM_ERR_TRUNCATED;
            }
            kind = *cur++;
            if (kind > 3u) {
                return WASM_ERR_INVALID_SECTION;
            }
            rc = leb128_decode_u32(&cur, end, &idx);
            if (rc != WASM_OK) {
                return rc;
            }
            (void)idx;
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION;
        }
    }

    /* Data: structure + i32 offset typing (bounds stay link-time). */
    if (has_data) {
        uint32_t count;
        cur = datap.data;
        end = datap.data + datap.len;
        rc = leb128_decode_u32(&cur, end, &count);
        if (rc != WASM_OK) {
            return rc;
        }
        if (count > (uint32_t)WASM_MAX_DATA_SEGMENTS) {
            return WASM_ERR_SECTION_OVERFLOW;
        }
        for (i = 0u; i < count; i++) {
            uint32_t memidx = 0u, len = 0u;
            uint8_t it;
            rc = leb128_decode_u32(&cur, end, &memidx);
            if (rc != WASM_OK) {
                return rc;
            }
            if (memidx != 0u) {
                return WASM_ERR_INVALID_SECTION;
            }
            rc = veval_init(&cur, end, &mc, mc.nimportglobals, &it);
            if (rc != WASM_OK) {
                return rc;
            }
            if (it != (uint8_t)WASM_TYPE_I32) {
                return WASM_ERR_INVALID_SECTION;
            }
            rc = leb128_decode_u32(&cur, end, &len);
            if (rc != WASM_OK) {
                return rc;
            }
            if (!vreadable(cur, end, len)) {
                return WASM_ERR_TRUNCATED;
            }
            cur += len;
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION;
        }
    }

    /* Elements: structure + i32 offset typing + funcidx range. */
    if (has_elem) {
        uint32_t count;
        uint32_t nfuncs = mc.nimports + mc.ndefined;
        cur = elemp.data;
        end = elemp.data + elemp.len;
        rc = leb128_decode_u32(&cur, end, &count);
        if (rc != WASM_OK) {
            return rc;
        }
        for (i = 0u; i < count; i++) {
            uint32_t tableidx = 0u, len = 0u, k;
            uint8_t it;
            rc = leb128_decode_u32(&cur, end, &tableidx);
            if (rc != WASM_OK) {
                return rc;
            }
            if (tableidx != 0u) {
                return WASM_ERR_INVALID_SECTION;
            }
            rc = veval_init(&cur, end, &mc, mc.nimportglobals, &it);
            if (rc != WASM_OK) {
                return rc;
            }
            if (it != (uint8_t)WASM_TYPE_I32) {
                return WASM_ERR_INVALID_SECTION;
            }
            rc = leb128_decode_u32(&cur, end, &len);
            if (rc != WASM_OK) {
                return rc;
            }
            for (k = 0u; k < len; k++) {
                uint32_t fidx = 0u;
                rc = leb128_decode_u32(&cur, end, &fidx);
                if (rc != WASM_OK) {
                    return rc;
                }
                if (fidx >= nfuncs) {
                    return WASM_ERR_INVALID_SECTION;
                }
            }
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION;
        }
    }

    /* Start: single index, []->[] signature (host case left to link). */
    if (has_start) {
        uint32_t funcidx = 0u;
        cur = startp.data;
        end = startp.data + startp.len;
        rc = leb128_decode_u32(&cur, end, &funcidx);
        if (rc != WASM_OK) {
            return rc;
        }
        if (cur != end) {
            return WASM_ERR_INVALID_SECTION;
        }
        if (funcidx >= mc.nimports + mc.ndefined) {
            return WASM_ERR_INVALID_SECTION;
        }
        if (funcidx >= mc.nimports) {
            uint32_t ti = mc.ftype[funcidx - mc.nimports];
            uint32_t pc = 0u, rcnt = 0u;
            rc = vtype_sig(&mc, ti, NULL, 0u, &pc, NULL, &rcnt);
            if (rc != WASM_OK) {
                return rc;
            }
            if (pc != 0u || rcnt != 0u) {
                return WASM_ERR_TYPE_MISMATCH_IMPORT;
            }
        }
    }

    /* Code: count match + per-body machine. */
    if (has_func || has_code) {
        uint32_t count = 0u;
        if (!has_func && has_code) {
            return WASM_ERR_INVALID_SECTION;
        }
        if (has_func && !has_code && mc.ndefined > 0u) {
            return WASM_ERR_INVALID_SECTION;
        }
        if (has_code) {
            cur = codep.data;
            end = codep.data + codep.len;
            rc = leb128_decode_u32(&cur, end, &count);
            if (rc != WASM_OK) {
                return rc;
            }
            if (count != mc.ndefined) {
                return WASM_ERR_INVALID_SECTION;
            }
            for (i = 0u; i < count; i++) {
                uint32_t body_size;
                const uint8_t *body, *body_end;
                uint32_t ti = mc.ftype[i];
                uint8_t pt[WASM_MAX_LOCALS_PER_FRAME];
                uint8_t rt = V_BLOCK_VOID;
                uint32_t npc = 0u, nrc = 0u;
                uint32_t q;
                rc = leb128_decode_u32(&cur, end, &body_size);
                if (rc != WASM_OK) {
                    return rc;
                }
                if (!vreadable(cur, end, body_size)) {
                    return WASM_ERR_TRUNCATED;
                }
                body = cur;
                body_end = cur + body_size;
                cur += body_size;
                rc = vtype_sig(&mc, ti, pt,
                               (uint32_t)WASM_MAX_LOCALS_PER_FRAME, &npc,
                               &rt, &nrc);
                if (rc != WASM_OK) {
                    return rc;
                }
                if (nrc > 1u ||
                    (uint64_t)npc > (uint64_t)WASM_MAX_LOCALS_PER_FRAME) {
                    return WASM_ERR_TYPE_MISMATCH_IMPORT;
                }
                v.nlocals = 0u;
                for (q = 0u; q < npc; q++) {
                    v.locals[v.nlocals++] = pt[q];
                }
                rc = vfunc_body(&v, body, body_end, pt, npc, rt, nrc);
                if (rc != WASM_OK) {
                    return rc;
                }
            }
            if (cur != end) {
                return WASM_ERR_INVALID_SECTION;
            }
        }
    }
    return WASM_OK;
}
