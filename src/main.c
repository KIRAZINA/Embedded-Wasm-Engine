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
#include "wasm_host.h"
#include "wasm_parser.h"
#include "wasm_validate.h"
#include "wasm_vm.h"

#define RUNNER_FILE_MAX (64u * 1024u)
#define RUNNER_ARENA_PAGES 1u
#define RUNNER_STEP_BUDGET 10000000u

static uint8_t g_file_buf[RUNNER_FILE_MAX];
static uint8_t g_arena[RUNNER_ARENA_PAGES * WASM_PAGE_SIZE];
static wasm_vm_t g_vm;
static wasm_memory_t g_mem;
static wasm_host_registry_t g_reg;

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
    printf("%d\n", (int)args[0].value.i32);
    return WASM_TRAP_OK;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s <file.wasm> [export] [i32 args...]\n", prog);
    fprintf(stderr, "       %s --dump <file.wasm>\n", prog);
}

/* Strict i32 parse: no trailing garbage, no overflow. */
static int parse_i32(const char *s, int32_t *out)
{
    long v;
    char *e = NULL;
    if (s == NULL || *s == '\0' || out == NULL) {
        return 0;
    }
    v = strtol(s, &e, 0);
    if (e == s || *e != '\0' || v < (long)INT32_MIN || v > (long)INT32_MAX) {
        return 0;
    }
    *out = (int32_t)v;
    return 1;
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
    int dump_mode = 0;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--dump") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 2;
        }
        dump_mode = 1;
        path = argv[2];
    } else {
        path = argv[1];
        if (argc >= 3) {
            export_name = argv[2];
        }
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
    printf("[load] %lu bytes from '%s'\n", (unsigned long)nread, path);

    /* b) Header + section map (Phase 1). */
    ec = wasm_parse_header(g_file_buf, nread, &mod);
    if (ec != WASM_OK) {
        fprintf(stderr, "error: bad header: %s\n", wasm_err_str(ec));
        return 1;
    }
    printf("[parse] header ok\n");

    /* Inspection only: describe the module, never execute it. */
    if (dump_mode) {
        return dump_module(g_file_buf, nread, &mod);
    }

    /* Validation is a precondition of the execution pipeline.  Keep
     * --dump structural-only, while normal execution rejects every invalid
     * module before host registration, linking, segment writes, or start. */
    ec = wasm_validate_module(&mod);
    if (ec != WASM_OK) {
        fprintf(stderr, "error: validation: %s\n", wasm_err_str(ec));
        return 1;
    }
    printf("[validate] module ok\n");

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
    printf("[link] %u function(s)\n", (unsigned)nfuncs);

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
    printf("[export] '%s' -> func %u\n", export_name, (unsigned)fidx);

    /* e) Push CLI args, enter, execute with watchdog, collect the result. */
    {
        const wasm_func_desc_t *d = &g_vm.funcs[fidx];
        uint32_t nargs = (argc > 3) ? (uint32_t)(argc - 3) : 0u;
        if (nargs != d->param_count) {
            fprintf(stderr, "error: '%s' takes %u arg(s), got %u\n",
                    export_name, (unsigned)d->param_count, (unsigned)nargs);
            return 2;
        }
        if (d->is_host) {
            /* Exported host function: invoke directly, no bytecode. */
            wasm_val_t hargs[WASM_MAX_LOCALS_PER_FRAME];
            wasm_val_t hrets[1];
            for (i = 0u; i < nargs; i++) {
                int32_t v = 0;
                if (!parse_i32(argv[3 + i], &v)) {
                    fprintf(stderr, "error: arg %u ('%s') is not an i32\n",
                            (unsigned)i, argv[3 + i]);
                    return 2;
                }
                hargs[i].type = WASM_TYPE_I32;
                hargs[i].value.i32 = v;
            }
            hrets[0].type = WASM_TYPE_I32;
            hrets[0].value.i32 = 0;
            tr = d->host_cb(&g_vm, hargs, nargs, hrets, d->return_count);
            if (tr != WASM_TRAP_OK) {
                fprintf(stderr, "error: host call trapped: %s\n",
                        wasm_trap_str(tr));
                return 1;
            }
            if (d->return_count > 0u) {
                print_val(&hrets[0]);
            } else {
                printf("ok (no result)\n");
            }
            return 0;
        }
        for (i = 0u; i < nargs; i++) {
            int32_t v = 0;
            wasm_val_t w;
            if (!parse_i32(argv[3 + i], &v)) {
                fprintf(stderr, "error: arg %u ('%s') is not an i32\n",
                        (unsigned)i, argv[3 + i]);
                return 2;
            }
            w.type = WASM_TYPE_I32;
            w.value.i32 = v;
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
            fprintf(stderr, "error: trapped: %s\n", wasm_trap_str(tr));
            return 1;
        }
        if (d->return_count > 0u) {
            tr = wasm_stack_pop(&g_vm.operands, &res);
            if (tr != WASM_TRAP_OK) {
                fprintf(stderr, "error: missing result: %s\n",
                        wasm_trap_str(tr));
                return 1;
            }
            print_val(&res);
        } else {
            printf("ok (no result)\n");
        }
    }
    return 0;
}
