/*
 * main.c — embedded CLI runner for the C11 WASM interpreter (Phase 6).
 *
 * Usage: wasm_runner <file.wasm> [export] [i32 args...]
 *        wasm_runner --dump <file.wasm>
 *
 * Pipeline: read file into a fixed static buffer -> parse header/sections
 * (Phase 1) -> register host callbacks -> instantiate (Phase 6 link) ->
 * optional linear memory -> resolve export -> push CLI args -> execute with
 * a step budget (watchdog) -> print status + return value.
 *
 * With --dump: print header, section table, type signatures and export
 * entries, then exit 0 without executing anything.
 *
 * Zero allocation: every buffer is static. Exit codes: 0 ok, 1 runtime or
 * link error, 2 usage / I/O error.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "leb128.h"
#include "version.h"
#include "wasm_host.h"
#include "wasm_parser.h"
#include "wasm_validate.h"
#include "wasm_vm.h"

#ifndef RUNNER_FILE_MAX
#define RUNNER_FILE_MAX (64u * 1024u) /* maximum CLI input file in bytes */
#endif
#ifndef RUNNER_ARENA_PAGES
#define RUNNER_ARENA_PAGES 1u /* pages reserved by the CLI memory arena */
#endif
#ifndef RUNNER_STEP_BUDGET
#define RUNNER_STEP_BUDGET 10000000u /* CLI execution watchdog steps */
#endif
#ifndef SESSION_ARENA_PAGES
#define SESSION_ARENA_PAGES 64u /* pages reserved by --session arena */
#endif

static int g_quiet;
static int g_result_line;

static uint8_t g_file_buf[RUNNER_FILE_MAX];
static uint8_t g_arena[RUNNER_ARENA_PAGES * WASM_PAGE_SIZE];
static wasm_vm_t g_vm;
static wasm_memory_t g_mem;
static wasm_host_registry_t g_reg;

/* --session state (independent of the one-shot path above). */
static uint8_t s_file_buf[RUNNER_FILE_MAX];
static uint8_t s_arena[SESSION_ARENA_PAGES * WASM_PAGE_SIZE];
static wasm_vm_t s_vm;
static wasm_memory_t s_mem;
static wasm_host_registry_t s_reg;
static wasm_module_t s_mod;
static int s_have_mod = 0;
static char s_line[2048];

/* Native host callback visible to guests as env.print_i32(i32). */
static wasm_trap_t host_print_i32(struct wasm_vm *vm,
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
    if (!g_result_line) printf("%d\n", (int)args[0].value.i32);
    return WASM_TRAP_OK;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--quiet] [--result-line] [--no-validate] <file.wasm> [export] [--arg TYPE:VALUE ...]\n", prog);
    fprintf(stderr, "       %s --dump <file.wasm>\n", prog);
    fprintf(stderr, "       %s --session <cmdfile>\n", prog);
    fprintf(stderr, "       %s --version\n", prog);
    fprintf(stderr, "note: modules are statically validated before linking;\n");
    fprintf(stderr, "      --no-validate skips that check (runtime traps still apply).\n");
}

/* Strict i32 parse: no trailing garbage, no overflow. */
static int parse_i32(const char *s, int32_t *out)
{
    long long v;
    char *e = NULL;
    if (s == NULL || *s == '\0' || out == NULL) {
        return 0;
    }
    v = strtoll(s, &e, 0);
    if (e == s || *e != '\0' || v < (long long)INT32_MIN || v > (long long)INT32_MAX) {
        return 0;
    }
    *out = (int32_t)v;
    return 1;
}

static int parse_i64(const char *s, int64_t *out)
{
    long long v;
    char *e = NULL;
    if (s == NULL || *s == '\0' || out == NULL) {
        return 0;
    }
    v = strtoll(s, &e, 0);
    if (e == s || *e != '\0') {
        return 0;
    }
    *out = (int64_t)v;
    return 1;
}

static int parse_float_arg(const char *s, int is_f32, wasm_val_t *out)
{
    char *e = NULL;
    if (s == NULL || *s == '\0' || out == NULL) {
        return 0;
    }
    if (strncmp(s, "0x", 2u) == 0 || strncmp(s, "0X", 2u) == 0) {
        unsigned long long bits = strtoull(s, &e, 16);
        unsigned long long max = is_f32 ? 0xFFFFFFFFULL : 0xFFFFFFFFFFFFFFFFULL;
        if (e == s + 2 || *e != '\0' || bits > max) {
            return 0;
        }
        out->type = is_f32 ? WASM_TYPE_F32 : WASM_TYPE_F64;
        if (is_f32) {
            uint32_t b = (uint32_t)bits;
            memcpy(&out->value.f32, &b, sizeof(b));
        } else {
            uint64_t b = (uint64_t)bits;
            memcpy(&out->value.f64, &b, sizeof(b));
        }
        return 1;
    }
    if (is_f32) {
        float v = strtof(s, &e);
        if (e == s || *e != '\0') {
            return 0;
        }
        out->type = WASM_TYPE_F32;
        out->value.f32 = v;
    } else {
        double v = strtod(s, &e);
        if (e == s || *e != '\0') {
            return 0;
        }
        out->type = WASM_TYPE_F64;
        out->value.f64 = v;
    }
    return 1;
}

static int parse_typed_arg(const char *s, wasm_val_t *out)
{
    const char *colon;
    if (s == NULL || out == NULL) {
        return 0;
    }
    colon = strchr(s, ':');
    if (colon == NULL || colon == s || colon[1] == '\0') {
        return 0;
    }
    if ((size_t)(colon - s) == 3u && strncmp(s, "i32", 3u) == 0) {
        int32_t v;
        if (!parse_i32(colon + 1, &v)) return 0;
        out->type = WASM_TYPE_I32; out->value.i32 = v; return 1;
    }
    if ((size_t)(colon - s) == 3u && strncmp(s, "i64", 3u) == 0) {
        int64_t v;
        if (!parse_i64(colon + 1, &v)) return 0;
        out->type = WASM_TYPE_I64; out->value.i64 = v; return 1;
    }
    if ((size_t)(colon - s) == 3u && strncmp(s, "f32", 3u) == 0) {
        return parse_float_arg(colon + 1, 1, out);
    }
    if ((size_t)(colon - s) == 3u && strncmp(s, "f64", 3u) == 0) {
        return parse_float_arg(colon + 1, 0, out);
    }
    return 0;
}

static void print_hex64(uint64_t bits, unsigned digits)
{
    static const char hex[] = "0123456789abcdef";
    char text[17];
    unsigned i;
    text[digits] = '\0';
    for (i = 0u; i < digits; i++) {
        text[digits - 1u - i] = hex[(unsigned)(bits & 0xFu)];
        bits >>= 4u;
    }
    printf("%s", text);
}

static void print_i64(int64_t v)
{
    /* Decimal render without printf length modifiers: keeps -Wpedantic
     * clean on toolchains whose printf archetype is ms_printf (MinGW),
     * where neither PRId64 ("I64d") nor "%lld" passes -Werror=format. */
    char buf[21]; /* "-9223372036854775808" + NUL */
    uint64_t u;
    int neg, i;
    neg = (v < 0);
    u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    i = 20;
    buf[20] = '\0';
    do {
        buf[--i] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u != 0u);
    if (neg) {
        buf[--i] = '-';
    }
    printf("result: %s (i64)\n", &buf[i]);
}

static void print_val(const wasm_val_t *v)
{
    switch (v->type) {
    case WASM_TYPE_I32:
        printf("result: %d (i32)\n", (int)v->value.i32);
        break;
    case WASM_TYPE_I64:
        print_i64(v->value.i64);
        break;
    case WASM_TYPE_F32:
        /* %.9g round-trips any f32; nan/inf print as such (C99). */
        printf("result: %.9g (f32)\n", (double)v->value.f32);
        break;
    case WASM_TYPE_F64:
        /* %.17g round-trips any f64. */
        printf("result: %.17g (f64)\n", v->value.f64);
        break;
    default:
        printf("result: <unknown type 0x%02x>\n", (unsigned)v->type);
        break;
    }
}

static void print_result_line(const wasm_val_t *v)
{
    char buf[21];
    uint64_t u;
    int neg;
    int pos;
    uint32_t b32;
    uint64_t b64;
    if (v == NULL) {
        printf("RESULT void\n");
        return;
    }
    switch (v->type) {
    case WASM_TYPE_I32: printf("RESULT i32 %d\n", (int)v->value.i32); break;
    case WASM_TYPE_I64:
        neg = v->value.i64 < 0;
        u = neg ? (uint64_t)(-(v->value.i64 + 1)) + 1u : (uint64_t)v->value.i64;
        pos = 20; buf[20] = '\0';
        do { buf[--pos] = (char)('0' + (u % 10u)); u /= 10u; } while (u != 0u);
        if (neg) buf[--pos] = '-';
        printf("RESULT i64 %s\n", &buf[pos]);
        break;
    case WASM_TYPE_F32:
        memcpy(&b32, &v->value.f32, sizeof(b32));
        printf("RESULT f32 0x"); print_hex64(b32, 8u); printf("\n"); break;
    case WASM_TYPE_F64:
        memcpy(&b64, &v->value.f64, sizeof(b64));
        printf("RESULT f64 0x"); print_hex64(b64, 16u); printf("\n"); break;
    default: printf("RESULT void\n"); break;
    }
}

/* ==================== --dump inspection mode ==================== */

static const char *section_name(uint8_t id)
{
    switch (id) {
    case (uint8_t)WASM_SECT_CUSTOM:    return "Custom";
    case (uint8_t)WASM_SECT_TYPE:      return "Type";
    case (uint8_t)WASM_SECT_IMPORT:    return "Import";
    case (uint8_t)WASM_SECT_FUNCTION:  return "Function";
    case (uint8_t)WASM_SECT_TABLE:     return "Table";
    case (uint8_t)WASM_SECT_MEMORY:    return "Memory";
    case (uint8_t)WASM_SECT_GLOBAL:    return "Global";
    case (uint8_t)WASM_SECT_EXPORT:    return "Export";
    case (uint8_t)WASM_SECT_START:     return "Start";
    case (uint8_t)WASM_SECT_ELEMENT:   return "Element";
    case (uint8_t)WASM_SECT_CODE:      return "Code";
    case (uint8_t)WASM_SECT_DATA:      return "Data";
    case (uint8_t)WASM_SECT_DATACOUNT: return "DataCount";
    default:                           return "?";
    }
}

static const char *valtype_name(uint8_t b)
{
    switch (b) {
    case (uint8_t)WASM_TYPE_I32: return "i32";
    case (uint8_t)WASM_TYPE_I64: return "i64";
    case (uint8_t)WASM_TYPE_F32: return "f32";
    case (uint8_t)WASM_TYPE_F64: return "f64";
    default:                     return "?";
    }
}

static const char *extkind_name(uint8_t k)
{
    switch (k) {
    case 0u:  return "function";
    case 1u:  return "table";
    case 2u:  return "memory";
    case 3u:  return "global";
    default:  return "?";
    }
}

/* Print one functype entry body: caller guarantees p < end. */
static int dump_one_type(const uint8_t **pp, const uint8_t *end, uint32_t idx)
{
    const uint8_t *p = *pp;
    uint32_t npc = 0u, nrc = 0u, k = 0u;
    uint8_t b;
    wasm_err_t e;
    if (p >= end || *p != 0x60u) {
        fprintf(stderr, "error: malformed type section (bad functype tag)\n");
        return 1;
    }
    p++;
    e = leb128_decode_u32(&p, end, &npc);
    if (e != WASM_OK) {
        fprintf(stderr, "error: malformed type section: %s\n", wasm_err_str(e));
        return 1;
    }
    printf("[dump]   functype %u: params[", (unsigned)idx);
    for (k = 0u; k < npc; k++) {
        if (p >= end) {
            fprintf(stderr, "error: malformed type section (cut params)\n");
            return 1;
        }
        b = *p++;
        printf("%s%s", k > 0u ? ", " : "", valtype_name(b));
    }
    e = leb128_decode_u32(&p, end, &nrc);
    if (e != WASM_OK) {
        fprintf(stderr, "error: malformed type section: %s\n", wasm_err_str(e));
        return 1;
    }
    printf("] results[");
    for (k = 0u; k < nrc; k++) {
        if (p >= end) {
            fprintf(stderr, "error: malformed type section (cut results)\n");
            return 1;
        }
        b = *p++;
        printf("%s%s", k > 0u ? ", " : "", valtype_name(b));
    }
    printf("]\n");
    *pp = p;
    return 0;
}

static int dump_types(const wasm_module_t *mod)
{
    wasm_section_header_t sec;
    const uint8_t *p, *end;
    uint32_t count = 0u, t = 0u;
    wasm_err_t e;
    if (wasm_find_section(mod, (uint8_t)WASM_SECT_TYPE, &sec) != WASM_OK) {
        printf("[dump] types: (no type section)\n");
        return 0;
    }
    p = sec.payload.data;
    end = p + sec.payload.len;
    e = leb128_decode_u32(&p, end, &count);
    if (e != WASM_OK) {
        fprintf(stderr, "error: malformed type section: %s\n", wasm_err_str(e));
        return 1;
    }
    printf("[dump] types: %u\n", (unsigned)count);
    for (t = 0u; t < count; t++) {
        if (dump_one_type(&p, end, t) != 0) {
            return 1;
        }
    }
    if (p != end) {
        fprintf(stderr, "error: malformed type section (trailing bytes)\n");
        return 1;
    }
    return 0;
}

static int dump_exports(const wasm_module_t *mod)
{
    wasm_section_header_t sec;
    const uint8_t *p, *end;
    uint32_t count = 0u, i = 0u;
    wasm_err_t e;
    if (wasm_find_section(mod, (uint8_t)WASM_SECT_EXPORT, &sec) != WASM_OK) {
        printf("[dump] exports: (no export section)\n");
        return 0;
    }
    p = sec.payload.data;
    end = p + sec.payload.len;
    e = leb128_decode_u32(&p, end, &count);
    if (e != WASM_OK) {
        fprintf(stderr, "error: malformed export section: %s\n", wasm_err_str(e));
        return 1;
    }
    printf("[dump] exports: %u\n", (unsigned)count);
    for (i = 0u; i < count; i++) {
        uint32_t nlen = 0u, kind = 0u, idx = 0u;
        const uint8_t *nm;
        e = leb128_decode_u32(&p, end, &nlen);
        if (e != WASM_OK) {
            fprintf(stderr, "error: malformed export section: %s\n", wasm_err_str(e));
            return 1;
        }
        if (nlen > (uint32_t)(end - p)) {
            fprintf(stderr, "error: malformed export section (cut name)\n");
            return 1;
        }
        nm = p;
        p += nlen;
        if (p >= end) {
            fprintf(stderr, "error: malformed export section (cut kind)\n");
            return 1;
        }
        kind = *p++;
        e = leb128_decode_u32(&p, end, &idx);
        if (e != WASM_OK) {
            fprintf(stderr, "error: malformed export section: %s\n", wasm_err_str(e));
            return 1;
        }
        printf("[dump]   \"%.*s\" kind=%s index=%u\n",
               (int)nlen, (const char *)nm,
               extkind_name((uint8_t)kind), (unsigned)idx);
    }
    if (p != end) {
        fprintf(stderr, "error: malformed export section (trailing bytes)\n");
        return 1;
    }
    return 0;
}

/* Parse is already validated; print header, section table, types, exports. */
static int dump_module(const uint8_t *buf, size_t len, const wasm_module_t *mod)
{
    uint32_t ver;
    const uint8_t *cursor;
    printf("[dump] magic: %02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3]);
    ver = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8u) |
          ((uint32_t)buf[6] << 16u) | ((uint32_t)buf[7] << 24u);
    printf("[dump] version: %u\n", (unsigned)ver);
    printf("[dump] size: %lu bytes\n", (unsigned long)len);
    printf("[dump] sections:\n");
    cursor = mod->sections_begin;
    while (cursor < mod->sections_end) {
        const uint8_t *hdr = cursor;
        wasm_section_header_t sec;
        wasm_err_t e = wasm_get_section(&cursor, mod->sections_end, &sec);
        if (e != WASM_OK) {
            fprintf(stderr, "error: malformed section table: %s\n", wasm_err_str(e));
            return 1;
        }
        printf("[dump]   id=%u name=%s header_off=%lu payload_off=%lu payload_len=%lu\n",
               (unsigned)sec.id, section_name(sec.id),
               (unsigned long)(hdr - buf),
               (unsigned long)(sec.payload.data - buf),
               (unsigned long)sec.payload.len);
    }
    if (dump_types(mod) != 0) {
        return 1;
    }
    if (dump_exports(mod) != 0) {
        return 1;
    }
    return 0;
}

/* ==================== --session batch mode ====================
 *
 * Machine protocol for the spec-suite harness (one reply line per command
 * line, always; empty/blank lines reply SESSION_ERROR to keep pairing):
 *   LOAD <path>                       -> LOADED <nfuncs> | LOAD_ERROR <ERR>
 *   INVOKE <nargs> <TYPE:VALUE ...> <export>
 *                                     -> RESULT ... | RESULT trap <NAME> |
 *                                        EXHAUSTED | INVOKE_ERROR <ERR>
 *   GET <export>                      -> RESULT ... | GET_ERROR <ERR>
 * Anything else                       -> SESSION_ERROR <what>
 *
 * One instance lives at a time; LOAD replaces it (fresh VM, registry, and
 * zeroed memory). Operand/call stacks reset before every INVOKE so a
 * trapped call never pollutes the next one; globals, table, and memory
 * persist across INVOKEs exactly like a live instance.
 */

static void session_load(const char *path)
{
    FILE *fp = NULL;
    long fsize = 0L;
    size_t nread = 0u;
    wasm_module_t mod;
    wasm_err_t ec;
    wasm_trap_t tr;
    uint32_t nfuncs = 0u;
    uint32_t mem_min = 0u;
    int mem_present = 0;
    uint64_t need;

    s_have_mod = 0; /* a failed LOAD leaves no usable instance behind */
    fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("LOAD_ERROR cannot-open\n");
        return;
    }
    if (fseek(fp, 0L, SEEK_END) != 0 ||
        (fsize = ftell(fp)) < 0L ||
        fseek(fp, 0L, SEEK_SET) != 0) {
        printf("LOAD_ERROR cannot-size\n");
        fclose(fp);
        return;
    }
    if (fsize == 0L || (uint64_t)fsize > (uint64_t)RUNNER_FILE_MAX) {
        printf("LOAD_ERROR file-too-large\n");
        fclose(fp);
        return;
    }
    nread = fread(s_file_buf, 1u, (size_t)fsize, fp);
    fclose(fp);
    if (nread != (size_t)fsize) {
        printf("LOAD_ERROR short-read\n");
        return;
    }
    ec = wasm_parse_header(s_file_buf, nread, &mod);
    if (ec != WASM_OK) {
        printf("LOAD_ERROR %s\n", wasm_err_str(ec));
        return;
    }
    s_mod = mod; /* borrowed pointers into s_file_buf (stable until LOAD) */
    s_have_mod = 1;
    ec = wasm_validate_module(&mod);
    if (ec != WASM_OK) {
        printf("LOAD_ERROR %s\n", wasm_err_str(ec));
        return;
    }
    tr = wasm_host_registry_init(&s_reg);
    if (tr != WASM_TRAP_OK) {
        printf("LOAD_ERROR %s\n", wasm_trap_str(tr));
        return;
    }
    tr = wasm_host_register_func(&s_reg, "env", "print_i32", 1u, 0u,
                                 host_print_i32);
    if (tr != WASM_TRAP_OK) {
        printf("LOAD_ERROR %s\n", wasm_trap_str(tr));
        return;
    }
    tr = wasm_vm_init(&s_vm);
    if (tr != WASM_TRAP_OK) {
        printf("LOAD_ERROR %s\n", wasm_trap_str(tr));
        return;
    }
    ec = wasm_get_memory_min(&mod, &mem_min, &mem_present);
    if (ec != WASM_OK) {
        printf("LOAD_ERROR %s\n", wasm_err_str(ec));
        return;
    }
    need = mem_present ? (uint64_t)mem_min * (uint64_t)WASM_PAGE_SIZE : 0u;
    if (need > (uint64_t)sizeof(s_arena)) {
        printf("LOAD_ERROR memory-too-large\n");
        return;
    }
    tr = wasm_memory_init(&s_mem, s_arena, (uint32_t)sizeof(s_arena),
                          mem_present ? mem_min : 0u,
                          (uint32_t)SESSION_ARENA_PAGES);
    if (tr != WASM_TRAP_OK) {
        printf("LOAD_ERROR %s\n", wasm_trap_str(tr));
        return;
    }
    tr = wasm_vm_bind_memory(&s_vm, &s_mem);
    if (tr != WASM_TRAP_OK) {
        printf("LOAD_ERROR %s\n", wasm_trap_str(tr));
        return;
    }
    ec = wasm_instantiate(&s_vm, &mod, &s_reg, &nfuncs);
    if (ec != WASM_OK) {
        printf("LOAD_ERROR %s\n", wasm_err_str(ec));
        return;
    }
    printf("LOADED %u\n", (unsigned)nfuncs);
}

static void session_invoke(const char *export_name, uint32_t nargs,
                           const wasm_val_t *args)
{
    wasm_section_header_t exsec;
    wasm_err_t ec;
    wasm_trap_t tr;
    uint32_t fidx = 0u, i;
    uint32_t budget = RUNNER_STEP_BUDGET;
    wasm_val_t res;
    wasm_slice_t pay;

    /* Fresh per-call stacks; instance state (globals/table/memory) stays. */
    tr = wasm_stack_init(&s_vm.operands);
    if (tr != WASM_TRAP_OK) {
        printf("INVOKE_ERROR %s\n", wasm_trap_str(tr));
        return;
    }
    tr = wasm_call_stack_init(&s_vm.calls);
    if (tr != WASM_TRAP_OK) {
        printf("INVOKE_ERROR %s\n", wasm_trap_str(tr));
        return;
    }
    if (!s_have_mod) {
        printf("INVOKE_ERROR no-module\n");
        return;
    }
    ec = wasm_find_section(&s_mod, (uint8_t)WASM_SECT_EXPORT, &exsec);
    if (ec != WASM_OK) {
        printf("INVOKE_ERROR %s\n", wasm_err_str(ec));
        return;
    }
    pay.data = exsec.payload.data;
    pay.len = exsec.payload.len;
    ec = wasm_get_export_func_index(&pay, export_name, &fidx);
    if (ec != WASM_OK) {
        printf("INVOKE_ERROR %s\n", wasm_err_str(ec));
        return;
    }
    {
        const wasm_func_desc_t *d = &s_vm.funcs[fidx];
        if (nargs != d->param_count) {
            printf("INVOKE_ERROR arg-count\n");
            return;
        }
        if (d->is_host) {
            wasm_val_t hrets[1];
            hrets[0].type = WASM_TYPE_I32;
            hrets[0].value.i32 = 0;
            tr = d->host_cb(&s_vm, args, nargs, hrets, d->return_count);
            if (tr != WASM_TRAP_OK) {
                printf("RESULT trap %s\n", wasm_trap_str(tr));
                return;
            }
            if (d->return_count > 0u) {
                print_result_line(&hrets[0]);
            } else {
                print_result_line(NULL);
            }
            return;
        }
        for (i = 0u; i < nargs; i++) {
            if (args[i].type != d->param_types[i]) {
                printf("INVOKE_ERROR arg-type\n");
                return;
            }
            tr = wasm_stack_push(&s_vm.operands, args[i]);
            if (tr != WASM_TRAP_OK) {
                printf("INVOKE_ERROR %s\n", wasm_trap_str(tr));
                return;
            }
        }
        tr = wasm_vm_enter_function(&s_vm, fidx, d->param_count,
                                    d->local_count, d->return_count);
        if (tr != WASM_TRAP_OK) {
            printf("INVOKE_ERROR %s\n", wasm_trap_str(tr));
            return;
        }
        tr = wasm_vm_attach_code(&s_vm, d->code, d->code_len);
        if (tr != WASM_TRAP_OK) {
            printf("INVOKE_ERROR %s\n", wasm_trap_str(tr));
            return;
        }
        tr = WASM_TRAP_OK;
        while (tr == WASM_TRAP_OK &&
               wasm_call_stack_depth(&s_vm.calls) > 0u) {
            wasm_frame_t *top = NULL;
            tr = wasm_call_stack_top(&s_vm.calls, &top);
            if (tr != WASM_TRAP_OK) {
                break;
            }
            if (top->code_ptr == NULL || top->code_end == NULL ||
                top->code_ptr >= top->code_end) {
                tr = wasm_vm_exit_function(&s_vm, top->return_count);
                continue;
            }
            if (budget == 0u) {
                printf("EXHAUSTED\n");
                return;
            }
            budget--;
            tr = wasm_vm_step(&s_vm);
        }
        if (tr != WASM_TRAP_OK) {
            printf("RESULT trap %s\n", wasm_trap_str(tr));
            return;
        }
        if (d->return_count > 0u) {
            tr = wasm_stack_pop(&s_vm.operands, &res);
            if (tr != WASM_TRAP_OK) {
                printf("INVOKE_ERROR %s\n", wasm_trap_str(tr));
                return;
            }
            print_result_line(&res);
        } else {
            print_result_line(NULL);
        }
    }
}

static void session_get(const char *export_name)
{
    wasm_section_header_t exsec;
    wasm_err_t ec;
    uint32_t gidx = 0u;
    wasm_slice_t pay;

    if (!s_have_mod) {
        printf("GET_ERROR no-module\n");
        return;
    }
    ec = wasm_find_section(&s_mod, (uint8_t)WASM_SECT_EXPORT, &exsec);
    if (ec != WASM_OK) {
        printf("GET_ERROR %s\n", wasm_err_str(ec));
        return;
    }
    pay.data = exsec.payload.data;
    pay.len = exsec.payload.len;
    ec = wasm_get_export_global_index(&pay, export_name, &gidx);
    if (ec != WASM_OK) {
        printf("GET_ERROR %s\n", wasm_err_str(ec));
        return;
    }
    if (gidx >= (uint32_t)WASM_MAX_GLOBALS || !s_vm.globals[gidx].valid) {
        printf("GET_ERROR bad-global\n");
        return;
    }
    print_result_line(&s_vm.globals[gidx].value);
}

/* Split "INVOKE <nargs> <args...> <export>": fixed-count typed tokens first,
 * the remainder of the line is the export name (may contain spaces). */
static void session_invoke_line(char *rest)
{
    char *p = rest;
    char *end = NULL;
    unsigned long n;
    uint32_t nargs, i;
    wasm_val_t args[WASM_MAX_LOCALS_PER_FRAME];

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    n = strtoul(p, &end, 10);
    if (end == p || n > (unsigned long)WASM_MAX_LOCALS_PER_FRAME) {
        printf("SESSION_ERROR bad-invoke-arity\n");
        return;
    }
    nargs = (uint32_t)n;
    p = end;
    for (i = 0u; i < nargs; i++) {
        char *tok;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            printf("SESSION_ERROR short-invoke-args\n");
            return;
        }
        tok = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
        if (!parse_typed_arg(tok, &args[i])) {
            printf("SESSION_ERROR bad-invoke-arg\n");
            return;
        }
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        printf("SESSION_ERROR missing-export\n");
        return;
    }
    session_invoke(p, nargs, args);
}

static int run_session(const char *cmdpath)
{
    FILE *fp = NULL;

    fp = fopen(cmdpath, "r");
    if (fp == NULL) {
        fprintf(stderr, "error: cannot open session file '%s'\n", cmdpath);
        return 2;
    }
    while (fgets(s_line, (int)sizeof(s_line), fp) != NULL) {
        size_t len = strlen(s_line);
        /* A full buffer with no newline means the line was truncated. */
        if (len > 0u && s_line[len - 1u] != '\n' && !feof(fp)) {
            int c;
            printf("SESSION_ERROR line-too-long\n");
            do {
                c = fgetc(fp);
            } while (c != '\n' && c != EOF);
            continue;
        }
        while (len > 0u &&
               (s_line[len - 1u] == '\n' || s_line[len - 1u] == '\r')) {
            s_line[--len] = '\0';
        }
        if (strncmp(s_line, "LOAD ", 5u) == 0) {
            session_load(s_line + 5);
        } else if (strncmp(s_line, "GET ", 4u) == 0) {
            session_get(s_line + 4);
        } else if (strncmp(s_line, "INVOKE ", 7u) == 0) {
            session_invoke_line(s_line + 7);
        } else {
            printf("SESSION_ERROR unknown-command\n");
        }
        fflush(stdout);
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path, *export_name = "main";
    FILE *fp = NULL;
    long fsize = 0L;
    size_t nread = 0u;
    wasm_module_t mod;
    wasm_section_header_t exsec;
    wasm_err_t ec;
    wasm_trap_t tr;
    uint32_t nfuncs = 0u, fidx = 0u, i;
    uint32_t mem_min = 0u;
    int mem_present = 0;
    uint64_t need;
    uint32_t budget = RUNNER_STEP_BUDGET;
    wasm_val_t res;
    wasm_val_t cli_args[WASM_MAX_LOCALS_PER_FRAME];
    uint32_t cli_nargs = 0u;
    int argi = 1;
    int dump_mode = 0;
    int no_validate = 0;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--version") == 0) {
            printf("%s\n", WASM_ENGINE_VERSION);
            return 0;
        }
        if (strcmp(argv[argi], "--quiet") == 0) {
            g_quiet = 1; argi++; continue;
        }
        if (strcmp(argv[argi], "--result-line") == 0) {
            g_result_line = 1; g_quiet = 1; argi++; continue;
        }
        if (strcmp(argv[argi], "--no-validate") == 0) {
            no_validate = 1; argi++; continue;
        }
        if (strcmp(argv[argi], "--dump") == 0) {
            dump_mode = 1; argi++; break;
        }
        if (strcmp(argv[argi], "--session") == 0) {
            if (argi + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            return run_session(argv[argi + 1]);
        }
        break;
    }
    if (argi >= argc) {
        usage(argv[0]);
        return 2;
    }
    path = argv[argi++];
    if (!dump_mode && argi < argc && argv[argi][0] != '-') {
        export_name = argv[argi++];
    }
    while (!dump_mode && argi < argc) {
        const char *value = argv[argi];
        wasm_val_t parsed;
        if (strcmp(value, "--quiet") == 0) {
            g_quiet = 1; argi++; continue;
        }
        if (strcmp(value, "--result-line") == 0) {
            g_result_line = 1; g_quiet = 1; argi++; continue;
        }
        if (strcmp(value, "--no-validate") == 0) {
            no_validate = 1; argi++; continue;
        }
        if (strcmp(value, "--arg") == 0) {
            if (argi + 1 >= argc || cli_nargs >= (uint32_t)WASM_MAX_LOCALS_PER_FRAME ||
                !parse_typed_arg(argv[argi + 1], &parsed)) {
                fprintf(stderr, "error: expected --arg TYPE:VALUE\n");
                return 2;
            }
            cli_args[cli_nargs++] = parsed; argi += 2u; continue;
        }
        if (strncmp(value, "--arg=", 6u) == 0) {
            if (cli_nargs >= (uint32_t)WASM_MAX_LOCALS_PER_FRAME ||
                !parse_typed_arg(value + 6, &parsed)) {
                fprintf(stderr, "error: expected --arg TYPE:VALUE\n");
                return 2;
            }
            cli_args[cli_nargs++] = parsed; argi++; continue;
        }
        if (strchr(value, ':') == NULL) {
            int32_t legacy;
            /* legacy positional argument */
            if (!parse_i32(value, &legacy)) {
                fprintf(stderr, "error: unknown argument '%s'\n", value);
                return 2;
            }
            parsed.type = WASM_TYPE_I32;
            parsed.value.i32 = legacy;
        } else if (!parse_typed_arg(value, &parsed)) {
            fprintf(stderr, "error: unknown argument '%s'\n", value);
            return 2;
        }
        if (cli_nargs >= (uint32_t)WASM_MAX_LOCALS_PER_FRAME) {
            fprintf(stderr, "error: unknown argument '%s'\n", value);
            return 2;
        }
        cli_args[cli_nargs++] = parsed;
        argi++;
    }

    /* a) File -> fixed static buffer (never malloc'd). */
    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return 2;
    }
    if (fseek(fp, 0L, SEEK_END) != 0 ||
        (fsize = ftell(fp)) < 0L ||
        fseek(fp, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "error: cannot size '%s'\n", path);
        fclose(fp);
        return 2;
    }
    if (fsize == 0L || (uint64_t)fsize > (uint64_t)RUNNER_FILE_MAX) {
        fprintf(stderr, "error: '%s' size %ld out of range (max %u)\n",
                path, fsize, (unsigned)RUNNER_FILE_MAX);
        fclose(fp);
        return 2;
    }
    nread = fread(g_file_buf, 1u, (size_t)fsize, fp);
    fclose(fp);
    fp = NULL;
    if (nread != (size_t)fsize) {
        fprintf(stderr, "error: short read on '%s'\n", path);
        return 2;
    }
    if (!g_quiet) printf("[load] %lu bytes from '%s'\n", (unsigned long)nread, path);

    /* b) Header + section map (Phase 1). */
    ec = wasm_parse_header(g_file_buf, nread, &mod);
    if (ec != WASM_OK) {
        fprintf(stderr, "error: bad header: %s\n", wasm_err_str(ec));
        return 1;
    }
    if (!g_quiet) printf("[parse] header ok\n");

    /* Inspection only: describe the module, never execute it. */
    if (dump_mode) {
        return dump_module(g_file_buf, nread, &mod);
    }

    /* Validation is a precondition of the execution pipeline (unless the
     * caller passes --no-validate). Keep --dump structural-only, while
     * normal execution rejects every invalid module before host
     * registration, linking, segment writes, or start. */
    if (!no_validate) {
        ec = wasm_validate_module(&mod);
        if (ec != WASM_OK) {
            fprintf(stderr, "error: validation: %s\n", wasm_err_str(ec));
            return 1;
        }
        if (!g_quiet) printf("[validate] module ok\n");
    }

    /* c) Host callbacks. */
    tr = wasm_host_registry_init(&g_reg);
    if (tr != WASM_TRAP_OK) {
        fprintf(stderr, "error: registry init: %s\n", wasm_trap_str(tr));
        return 1;
    }
    tr = wasm_host_register_func(&g_reg, "env", "print_i32", 1u, 0u,
                                 host_print_i32);
    if (tr != WASM_TRAP_OK) {
        fprintf(stderr, "error: register print_i32: %s\n", wasm_trap_str(tr));
        return 1;
    }

    /* Link everything into the VM func table. */
    tr = wasm_vm_init(&g_vm);
    if (tr != WASM_TRAP_OK) {
        fprintf(stderr, "error: vm init: %s\n", wasm_trap_str(tr));
        return 1;
    }

    /* Optional linear memory (app-owned arena). It must be bound BEFORE
     * instantiate: data segments are applied to it and start may use it. */
    ec = wasm_get_memory_min(&mod, &mem_min, &mem_present);
    if (ec != WASM_OK) {
        fprintf(stderr, "error: memory section: %s\n", wasm_err_str(ec));
        return 1;
    }
    need = mem_present ? (uint64_t)mem_min * (uint64_t)WASM_PAGE_SIZE : 0u;
    if (need > (uint64_t)sizeof(g_arena)) {
        fprintf(stderr, "error: module needs %lu memory bytes, have %u\n",
                (unsigned long)need, (unsigned)sizeof(g_arena));
        return 1;
    }
    tr = wasm_memory_init(&g_mem, g_arena, (uint32_t)sizeof(g_arena),
                          mem_present ? mem_min : 0u,
                          (uint32_t)RUNNER_ARENA_PAGES);
    if (tr != WASM_TRAP_OK) {
        fprintf(stderr, "error: memory init: %s\n", wasm_trap_str(tr));
        return 1;
    }
    tr = wasm_vm_bind_memory(&g_vm, &g_mem);
    if (tr != WASM_TRAP_OK) {
        fprintf(stderr, "error: memory bind: %s\n", wasm_trap_str(tr));
        return 1;
    }
    /* Instantiate: links imports, defines funcs/globals, applies data
     * segments to the bound memory, then runs the start function. */
    ec = wasm_instantiate(&g_vm, &mod, &g_reg, &nfuncs);
    if (ec != WASM_OK) {
        fprintf(stderr, "error: instantiate: %s\n", wasm_err_str(ec));
        return 1;
    }
    if (!g_quiet) printf("[link] %u function(s)\n", (unsigned)nfuncs);

    /* d) Resolve the export. */
    ec = wasm_find_section(&mod, (uint8_t)WASM_SECT_EXPORT, &exsec);
    if (ec != WASM_OK) {
        fprintf(stderr, "error: no export section (%s)\n", wasm_err_str(ec));
        return 1;
    }
    {
        wasm_slice_t pay;
        pay.data = exsec.payload.data;
        pay.len = exsec.payload.len;
        ec = wasm_get_export_func_index(&pay, export_name, &fidx);
    }
    if (ec != WASM_OK) {
        fprintf(stderr, "error: export '%s': %s\n", export_name,
                wasm_err_str(ec));
        return 1;
    }
    if (fidx >= nfuncs) {
        fprintf(stderr, "error: export '%s' -> func %u out of range (%u)\n",
                export_name, (unsigned)fidx, (unsigned)nfuncs);
        return 1;
    }
    if (!g_quiet) printf("[export] '%s' -> func %u\n", export_name, (unsigned)fidx);

    /* e) Push CLI args, enter, execute with watchdog, collect the result. */
    {
        const wasm_func_desc_t *d = &g_vm.funcs[fidx];
        uint32_t nargs = cli_nargs;
        if (nargs != d->param_count) {
            fprintf(stderr, "error: '%s' takes %u arg(s), got %u\n",
                    export_name, (unsigned)d->param_count, (unsigned)nargs);
            return 2;
        }
        if (d->is_host) {
            /* Exported host function: invoke directly, no bytecode. */
            wasm_val_t hargs[WASM_MAX_LOCALS_PER_FRAME];
            wasm_val_t hrets[1];
            for (i = 0u; i < nargs; i++) hargs[i] = cli_args[i];
            hrets[0].type = WASM_TYPE_I32;
            hrets[0].value.i32 = 0;
            tr = d->host_cb(&g_vm, hargs, nargs, hrets, d->return_count);
            if (tr != WASM_TRAP_OK) {
                if (g_result_line) printf("RESULT trap %s\n", wasm_trap_str(tr));
                else fprintf(stderr, "error: host call trapped: %s\n", wasm_trap_str(tr));
                return 1;
            }
            if (d->return_count > 0u) {
                if (g_result_line) print_result_line(&hrets[0]);
                else print_val(&hrets[0]);
            } else {
                if (g_result_line) printf("RESULT void\n");
                else printf("ok (no result)\n");
            }
            return 0;
        }
        for (i = 0u; i < nargs; i++) {
            wasm_val_t w = cli_args[i];
            if (w.type != d->param_types[i]) {
                fprintf(stderr, "error: arg %u type does not match parameter\n",
                        (unsigned)i);
                return 2;
            }
            tr = wasm_stack_push(&g_vm.operands, w);
            if (tr != WASM_TRAP_OK) {
                fprintf(stderr, "error: push arg: %s\n", wasm_trap_str(tr));
                return 1;
            }
        }
        tr = wasm_vm_enter_function(&g_vm, fidx, d->param_count, d->local_count,
                                    d->return_count);
        if (tr != WASM_TRAP_OK) {
            fprintf(stderr, "error: enter: %s\n", wasm_trap_str(tr));
            return 1;
        }
        tr = wasm_vm_attach_code(&g_vm, d->code, d->code_len);
        if (tr != WASM_TRAP_OK) {
            fprintf(stderr, "error: attach: %s\n", wasm_trap_str(tr));
            return 1;
        }
        /* Step loop: auto-exit finished frames, watchdog aborts hangers. */
        tr = WASM_TRAP_OK;
        while (tr == WASM_TRAP_OK &&
               wasm_call_stack_depth(&g_vm.calls) > 0u) {
            wasm_frame_t *top = NULL;
            tr = wasm_call_stack_top(&g_vm.calls, &top);
            if (tr != WASM_TRAP_OK) {
                break;
            }
            if (top->code_ptr == NULL || top->code_end == NULL ||
                top->code_ptr >= top->code_end) {
                tr = wasm_vm_exit_function(&g_vm, top->return_count);
                continue;
            }
            if (budget == 0u) {
                fprintf(stderr, "error: step budget exhausted (hang?)\n");
                return 1;
            }
            budget--;
            tr = wasm_vm_step(&g_vm);
        }
        if (tr != WASM_TRAP_OK) {
            if (g_result_line) printf("RESULT trap %s\n", wasm_trap_str(tr));
            else fprintf(stderr, "error: trapped: %s\n", wasm_trap_str(tr));
            return 1;
        }
        if (d->return_count > 0u) {
            tr = wasm_stack_pop(&g_vm.operands, &res);
            if (tr != WASM_TRAP_OK) {
                fprintf(stderr, "error: missing result: %s\n",
                        wasm_trap_str(tr));
                return 1;
            }
            if (g_result_line) print_result_line(&res);
            else print_val(&res);
        } else {
            if (g_result_line) printf("RESULT void\n");
            else printf("ok (no result)\n");
        }
    }
    return 0;
}
