#ifndef WASM_VALIDATE_H
#define WASM_VALIDATE_H

/*
 * wasm_validate.h — Phase 5 static module validation (WebAssembly 1.0).
 *
 * Implements the spec's validation algorithm (abstract ctrl-frame +
 * value-stack machine) over every function body, plus module-level
 * structural checks. Zero allocation: both abstract stacks are fixed
 * arrays (see WASM_VALIDATE_STACK_MAX / WASM_MAX_CONTROL_STACK_DEPTH).
 *
 * Error contract (MALFORMED vs INVALID):
 * - Encoding/structure defects keep the historic codes (TRUNCATED,
 *   LEB_*, SECTION_OVERFLOW, INVALID_SECTION, ...), chosen to match
 *   what wasm_instantiate() already reports for the same bytes.
 * - Typing-rule violations use the WASM_ERR_INVALID_* family:
 *     INVALID_TYPE   value-stack underflow/mismatch, select/call/return
 *                    arity, if conditions, call_indirect operands/range
 *     INVALID_LABEL  br/br_if/br_table label out of range
 *     INVALID_BLOCK  block nesting/structure/result errors
 *     INVALID_LOCAL  local index out of range
 *     INVALID_GLOBAL global index/mutability errors
 *     INVALID_ALIGN  alignment exceeds the access size
 * - Opcodes the interpreter cannot execute (nop, drop, i64 comparisons,
 *   sub-word/float memory ops, saturating conversions, ...) are rejected
 *   as INVALID_SECTION: undecodable into engine semantics.
 *
 * Engine-convention notes (validated to match execution, not bare spec):
 * - Function bodies WITHOUT a trailing 0x0B are accepted (implicit end);
 *   a trailing 0x0B closing an empty control stack is also accepted.
 * - Block result types accept all four MVP valtypes (void counts as
 *   no result); arity is at most one (single result byte).
 * - Locals carry their declared types; global.get of an imported
 *   immutable inside init/offset exprs is accepted (MVP rule).
 */

#include <stddef.h>
#include <stdint.h>

#include "wasm_parser.h"
#include "wasm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validate a parsed module WITHOUT executing anything: no code runs, no
 * data/element bytes are written, no host callbacks fire, no VM state is
 * touched. NULL module -> WASM_ERR_NULL. */
wasm_err_t wasm_validate_module(const wasm_module_t *mod);

#ifdef __cplusplus
}
#endif

#endif /* WASM_VALIDATE_H */
