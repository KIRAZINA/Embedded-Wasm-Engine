# Fixed limits and configuration

The engine never allocates dynamically. These preprocessor constants control
the amount of static storage or bounded work available to a build. Every value
has a documented default and may be overridden with `-DNAME=value` before the
relevant translation units are compiled. Increasing a storage limit increases
the containing object or executable footprint; reducing one can make otherwise
valid modules fail with a typed capacity error or trap.

| Constant | Default | What it bounds | Footprint impact | Override example |
|---|---:|---|---|---|
| `WASM_PAGE_SIZE` | 65536 bytes | One MVP linear-memory page | Changes memory backing-array sizing | `-DWASM_PAGE_SIZE=65536u` |
| `WASM_MEMORY_MAX_PAGES_ABSOLUTE` | 65536 pages | Absolute memory-page sanity ceiling | Parser/instance checks only | `-DWASM_MEMORY_MAX_PAGES_ABSOLUTE=1024u` |
| `WASM_STACK_MAX_DEPTH` | 1024 values | Runtime operand stack | Increases `wasm_stack_t` and `wasm_vm_t` | `-DWASM_STACK_MAX_DEPTH=512u` |
| `WASM_MAX_CALL_STACK_DEPTH` | 64 frames | Active function calls | Increases `wasm_call_stack_t` and `wasm_vm_t` | `-DWASM_MAX_CALL_STACK_DEPTH=16u` |
| `WASM_MAX_LOCALS_PER_FRAME` | 32 locals | Locals in one frame and CLI arguments | Increases each frame and `wasm_vm_t` | `-DWASM_MAX_LOCALS_PER_FRAME=16u` |
| `WASM_MAX_CONTROL_STACK_DEPTH` | 32 frames | Structured-control nesting | Increases control storage in each frame | `-DWASM_MAX_CONTROL_STACK_DEPTH=16u` |
| `WASM_MAX_FUNCTIONS` | 32 functions | Imported plus defined function slots | Increases VM function metadata | `-DWASM_MAX_FUNCTIONS=64u` |
| `WASM_MAX_BR_TABLE_LABELS` | 256 labels | Non-default labels in one `br_table` | Bounds temporary branch decoding | `-DWASM_MAX_BR_TABLE_LABELS=64u` |
| `WASM_MAX_GLOBALS` | 32 globals | Imported plus defined globals | Increases VM global storage | `-DWASM_MAX_GLOBALS=64u` |
| `WASM_MAX_HOST_GLOBALS` | 16 globals | Host-provided global bindings | Increases host registry storage | `-DWASM_MAX_HOST_GLOBALS=32u` |
| `WASM_MAX_DATA_SEGMENTS` | 32 segments | Data segments accepted by validation/linking | Bounds segment metadata/work | `-DWASM_MAX_DATA_SEGMENTS=64u` |
| `WASM_MAX_TABLE_SIZE` | 128 entries | MVP funcref table entries | Increases VM table storage | `-DWASM_MAX_TABLE_SIZE=256u` |
| `WASM_VALIDATE_STACK_MAX` | 1024 values | Validator abstract value stack | Increases validator local storage | `-DWASM_VALIDATE_STACK_MAX=512u` |
| `WASM_MAX_HOST_FUNCS` | 16 callbacks | Host callback registrations | Increases `wasm_host_registry_t` | `-DWASM_MAX_HOST_FUNCS=32u` |
| `WASM_HOST_NAME_MAX` | 32 bytes | Module/name buffers, including NUL | Increases each host entry | `-DWASM_HOST_NAME_MAX=64u` |
| `RUNNER_FILE_MAX` | 65536 bytes | CLI input file buffer | Increases CLI static buffer | `-DRUNNER_FILE_MAX=131072u` |
| `RUNNER_ARENA_PAGES` | 1 page | CLI linear-memory arena | Increases CLI static arena by 64 KiB/page | `-DRUNNER_ARENA_PAGES=2u` |
| `RUNNER_STEP_BUDGET` | 10000000 steps | CLI execution watchdog | No material static footprint | `-DRUNNER_STEP_BUDGET=1000000u` |
| `FUZZ_MAX_BYTES` | 256 bytes | Fuzzer input buffer | Increases fuzzer static buffer | `-DFUZZ_MAX_BYTES=1024u` |
| `FUZZ_PAGE_COUNT` | 1 page | Fuzzer memory arena | Increases fuzzer static arena | `-DFUZZ_PAGE_COUNT=2u` |
| `FUZZ_STEP_BUDGET` | 10 steps | Steps per fuzz case | No material static footprint | `-DFUZZ_STEP_BUDGET=100u` |
| `STM32F4_VM_BUDGET_BYTES` | 114688 bytes | Compile-time STM32F4 VM-size assertion | No runtime footprint | `-DSTM32F4_VM_BUDGET_BYTES=131072u` |

## Reference footprint

On the reference MinGW build, `make size-report` measured:

```text
wasm_vm_t=105008 bytes
wasm_host_registry_t=3584 bytes
wasm_memory_t=24 bytes
engine_static_footprint=108616 bytes
```

`tools/size_report.c` contains a compile-time assertion that `wasm_vm_t`
fits the documented 112 KiB STM32F4 VM budget. The CLI and guest memory arena
are application-level storage in addition to the engine object.

## Import and export support (MVP subset)

| Kind | Import | Export |
|---|---|---|
| Function | Supported via the host registry (signature-checked) | Supported (resolved by name) |
| Table | Rejected (`WASM_ERR_IMPORTED_TABLES_UNSUPPORTED`): the host has no table concept | Rejected unless exactly one funcref table is defined; execution never consults export entries |
| Memory | Rejected (`WASM_ERR_IMPORTED_MEMORIES_UNSUPPORTED`): the host has no memory object to bind — accepting would silently misdirect `memory.size` / `memory.grow` / data segments | Accepted but inert (memory ops use the bound memory regardless of exports) |
| Global | Supported via the host registry (type-checked) | Accepted (CLI name resolution is function-only) |

At most one table section and at most one memory section, and at most one
defined table / memory: anything more is rejected as malformed
(`WASM_ERR_INVALID_SECTION`), never silently shadowed by first-match.
