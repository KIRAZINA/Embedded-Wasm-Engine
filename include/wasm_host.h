#ifndef WASM_HOST_H
#define WASM_HOST_H

/*
 * wasm_host.h — Phase 6 host integration: native callbacks, import/export
 * linkage, and whole-module instantiation into the VM's function table.
 *
 * - Zero allocation: the registry embeds all entries (fixed char buffers,
 *   borrowed callback pointers); all link outputs go to caller-provided
 *   arrays or the VM's own pre-allocated func table.
 * - Index-space layout after instantiate(): [0, nimports) are host-bound
 *   functions, [nimports, nimports+ndefined) are WASM-defined functions —
 *   matching the MVP rule that imports precede definitions. Globals follow
 *   the same rule: imported globals first, then defined ones.
 * - MVP subset: function/global imports/exports (function exports only for
 *   lookup). Memory imports are parsed-and-skipped; table imports are
 *   rejected (WASM_ERR_IMPORTED_TABLES_UNSUPPORTED: the host has no table
 *   concept); multi-result or >32-local signatures are
 *   rejected (engine capability ceiling, reported as TYPE_MISMATCH_IMPORT).
 */

#include <stddef.h>
#include <stdint.h>

#include "wasm_parser.h"
#include "wasm_types.h"
#include "wasm_vm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WASM_MAX_HOST_FUNCS 16
#define WASM_HOST_NAME_MAX 32 /* bytes incl. NUL */

typedef struct {
    char module[WASM_HOST_NAME_MAX];
    char name[WASM_HOST_NAME_MAX];
    uint32_t param_count;
    uint32_t return_count;
    wasm_host_func_t func;
    int used;
    /* Full signature for call_indirect checks (only the first
     * param_count bytes valid; return_type 0 when resultless).
     * has_types == 0 (plain register_func) means "counts only": such
     * entries can never satisfy an indirect signature check. */
    uint8_t param_types[WASM_MAX_LOCALS_PER_FRAME];
    uint8_t return_type;
    int has_types;
} wasm_host_entry_t;

typedef struct {
    char module[WASM_HOST_NAME_MAX];
    char name[WASM_HOST_NAME_MAX];
    wasm_valtype_t type;
    int mut; /* 0 = immutable, 1 = mutable */
    wasm_val_t value;
    int used;
} wasm_host_global_t;

typedef struct {
    wasm_host_entry_t entries[WASM_MAX_HOST_FUNCS];
    wasm_host_global_t globals[WASM_MAX_HOST_GLOBALS];
} wasm_host_registry_t;

wasm_trap_t wasm_host_registry_init(wasm_host_registry_t *r);

/* Register one native callback. Name/module longer than 31 chars, a NULL
 * callback, or a full table is rejected (no partial registration). */
wasm_trap_t wasm_host_register_func(wasm_host_registry_t *r,
                                    const char *module, const char *name,
                                    uint32_t param_count, uint32_t return_count,
                                    wasm_host_func_t func);

/* Register one native callback WITH its full MVP signature for
 * call_indirect checking. param_types/ret_types borrow on entry and are
 * copied (NULL with nonzero count rejected); every byte must be a valid
 * MVP valtype, param_count within the locals cap, return_count at most 1.
 * Counts/names/callback follow the same rules as register_func. */
wasm_trap_t wasm_host_register_func_typed(wasm_host_registry_t *r,
                                          const char *module, const char *name,
                                          const wasm_valtype_t *param_types,
                                          uint32_t param_count,
                                          const wasm_valtype_t *ret_types,
                                          uint32_t return_count,
                                          wasm_host_func_t func);

/* Register one host-provided global (a copy of *value is stored; the
 * caller's pointer is never retained). type must be a valid MVP valtype,
 * mut 0/1, value non-NULL with value->type == type; names follow the same
 * 31-char rule as funcs. Full table or bad args -> INVALID_ARGUMENT. */
wasm_trap_t wasm_host_register_global(wasm_host_registry_t *r,
                                      const char *module, const char *name,
                                      wasm_valtype_t type, int mut,
                                      const wasm_val_t *value);

/* Look up a function export by exact name (kind must be 0x00).
 * Absent/empty payload or no match -> WASM_ERR_EXPORT_NOT_FOUND. */
wasm_err_t wasm_get_export_func_index(const wasm_slice_t *export_payload,
                                      const char *name, uint32_t *out_idx);

/* Map each function import (in order) to its registry slot.
 * type_payload may be NULL (signature check skipped); otherwise each
 * import's functype counts must equal the registry entry's, else
 * WASM_ERR_TYPE_MISMATCH_IMPORT. Unknown names ->
 * WASM_ERR_UNRESOLVED_IMPORT. More imports than cap ->
 * WASM_ERR_SECTION_OVERFLOW. */
wasm_err_t wasm_bind_imports(const wasm_slice_t *import_payload,
                             const wasm_slice_t *type_payload,
                             const wasm_host_registry_t *reg,
                             uint32_t *out_reg_idx, uint32_t cap,
                             uint32_t *out_nfuncs);

/* Map each global import (in order) to its registry slot, skipping all
 * non-global imports. The descriptor's valtype AND mutability must equal
 * the registry entry's, else WASM_ERR_TYPE_MISMATCH_IMPORT. Unknown names
 * (or NULL reg) -> WASM_ERR_UNRESOLVED_IMPORT. More global imports than
 * cap -> WASM_ERR_SECTION_OVERFLOW. */
wasm_err_t wasm_bind_global_imports(const wasm_slice_t *import_payload,
                                    const wasm_host_registry_t *reg,
                                    uint32_t *out_reg_idx, uint32_t cap,
                                    uint32_t *out_nglobals);

/* Link a parsed module into the VM: host-bind func imports to
 * funcs[0..ni), import globals to globals[0..ng), define WASM-code funcs
 * and globals after them. Data segments are applied to the already-bound
 * vm->mem (NULL memory + any segment -> DATA_OUT_OF_BOUNDS); a start
 * function ([]->[] only) runs last. Any data/start failure aborts with a
 * typed error and may leave committed tables behind: re-init the VM before
 * reuse after a failed instantiation. The VM must already be
 * wasm_vm_init'ed (never re-initialized here). Memory OWNERSHIP stays with
 * the host: bind it with wasm_vm_bind_memory() BEFORE calling this.
 * out_nfuncs (optional, may be NULL) receives ni + ndefined. */
wasm_err_t wasm_instantiate(wasm_vm_t *vm, const wasm_module_t *mod,
                            const wasm_host_registry_t *reg,
                            uint32_t *out_nfuncs);

/* Read the declared minimum of the first memory (if any).
 * Absent/empty section -> *out_present = 0, *out_min = 0, WASM_OK. */
wasm_err_t wasm_get_memory_min(const wasm_module_t *mod,
                               uint32_t *out_min, int *out_present);

#ifdef __cplusplus
}
#endif

#endif /* WASM_HOST_H */
