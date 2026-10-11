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
 *   matching the MVP rule that imports precede definitions.
 * - MVP subset: function imports/exports only. Table/memory/global imports
 *   are parsed-and-skipped; multi-result or >32-local signatures are
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
} wasm_host_entry_t;

typedef struct {
    wasm_host_entry_t entries[WASM_MAX_HOST_FUNCS];
} wasm_host_registry_t;

wasm_trap_t wasm_host_registry_init(wasm_host_registry_t *r);

/* Register one native callback. Name/module longer than 31 chars, a NULL
 * callback, or a full table is rejected (no partial registration). */
wasm_trap_t wasm_host_register_func(wasm_host_registry_t *r,
                                    const char *module, const char *name,
                                    uint32_t param_count, uint32_t return_count,
                                    wasm_host_func_t func);

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

/* Link a parsed module into the VM: host-bind func imports to
 * funcs[0..ni), define WASM-code funcs after them. The VM must already be
 * wasm_vm_init'ed (never re-initialized here). Memory/data/start sections
 * are intentionally left to the host (see wasm_get_memory_min).
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
