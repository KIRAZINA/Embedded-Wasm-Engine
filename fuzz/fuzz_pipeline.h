#ifndef FUZZ_PIPELINE_H
#define FUZZ_PIPELINE_H

/*
 * fuzz_pipeline.h — shared, zero-allocation fuzzing pipeline.
 *
 * One call runs a candidate module image through every engine stage that
 * a hostile input can reach:
 *   1. wasm_parse_header   (magic / version / length)
 *   2. section walk        (wasm_get_section over every section header)
 *   3. wasm_validate_module (static typing + structural validation)
 *   4. wasm_instantiate     (linking, data/element segments, start)
 *   5. execution            (a bounded number of VM steps)
 *
 * Every buffer is a file-scope static array: the pipeline never calls
 * malloc/calloc/realloc and never grows a buffer at run time.
 *
 * The return value is the number of stages that completed (diagnostics
 * only). A crash -- not a return value -- is the fuzzing signal.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Engine-visible caps: the pipeline never exceeds the engine's own
 * module size limit, and uses a single memory page for execution. */
#ifndef FUZZ_MAX_MODULE_BYTES
#define FUZZ_MAX_MODULE_BYTES 65536u
#endif
#ifndef FUZZ_MEM_PAGES
#define FUZZ_MEM_PAGES 1u
#endif
#ifndef FUZZ_STEP_BUDGET
#define FUZZ_STEP_BUDGET 256u
#endif

/* Run one candidate module image through the full pipeline.
 * Returns the number of pipeline stages that ran to completion. */
int wasm_fuzz_pipeline(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FUZZ_PIPELINE_H */
