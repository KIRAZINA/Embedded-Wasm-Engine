#ifndef WASM_TYPES_H
#define WASM_TYPES_H

/*
 * wasm_types.h — Phases 1-6 shared types for the ultra-lightweight WASM interpreter.
 *
 * Standard: C11. No dynamic allocation. All parsing is zero-copy: slices and
 * section headers are pointer offsets into the original byte buffer.
 *
 * Endianness: input is ALWAYS treated as a little-endian byte stream and is
 * decoded with explicit shifts/masks. No raw multi-byte casts are used, so
 * this header (and the parser built on it) is endianness-independent.
 */

#include <stddef.h>
#include <stdint.h>

/* ---- Binary format constants (WASM MVP core, section 5.5) ---- */
#define WASM_MAGIC_0 0x00u
#define WASM_MAGIC_1 0x61u /* 'a' */
#define WASM_MAGIC_2 0x73u /* 's' */
#define WASM_MAGIC_3 0x6Du /* 'm' */
#define WASM_VERSION 0x01u /* uint32 LE = 01 00 00 00 */
#define WASM_HEADER_SIZE 8u

/* ---- Error codes ---- */
typedef enum {
    WASM_OK = 0,
    WASM_ERR_NULL,            /* NULL pointer argument */
    WASM_ERR_TOO_SHORT,       /* buffer shorter than 8-byte header */
    WASM_ERR_MAGIC,           /* magic bytes mismatch */
    WASM_ERR_VERSION,         /* version mismatch */
    WASM_ERR_TRUNCATED,       /* buffer ends mid-structure (generic) */
    WASM_ERR_LEB_TRUNCATED,   /* LEB128 stream ends before terminator */
    WASM_ERR_LEB_OVERFLOW,    /* LEB128 value does not fit target type */
    WASM_ERR_SECTION_OVERFLOW,/* section payload extends past buffer end */
    WASM_ERR_INVALID_SECTION, /* section id out of range */
    WASM_ERR_UNRESOLVED_IMPORT,  /* import has no registered host function */
    WASM_ERR_EXPORT_NOT_FOUND,   /* no export entry matches the given name */
    WASM_ERR_TYPE_MISMATCH_IMPORT /* import/defined signature the engine cannot honor */
} wasm_err_t;

/* Human-readable error name. Zero-alloc: returns string literals. */
static inline const char *wasm_err_str(wasm_err_t e)
{
    switch (e) {
    case WASM_OK:                  return "WASM_OK";
    case WASM_ERR_NULL:            return "WASM_ERR_NULL";
    case WASM_ERR_TOO_SHORT:       return "WASM_ERR_TOO_SHORT";
    case WASM_ERR_MAGIC:           return "WASM_ERR_MAGIC";
    case WASM_ERR_VERSION:         return "WASM_ERR_VERSION";
    case WASM_ERR_TRUNCATED:       return "WASM_ERR_TRUNCATED";
    case WASM_ERR_LEB_TRUNCATED:   return "WASM_ERR_LEB_TRUNCATED";
    case WASM_ERR_LEB_OVERFLOW:    return "WASM_ERR_LEB_OVERFLOW";
    case WASM_ERR_SECTION_OVERFLOW:return "WASM_ERR_SECTION_OVERFLOW";
    case WASM_ERR_INVALID_SECTION: return "WASM_ERR_INVALID_SECTION";
    case WASM_ERR_UNRESOLVED_IMPORT: return "WASM_ERR_UNRESOLVED_IMPORT";
    case WASM_ERR_EXPORT_NOT_FOUND:  return "WASM_ERR_EXPORT_NOT_FOUND";
    case WASM_ERR_TYPE_MISMATCH_IMPORT: return "WASM_ERR_TYPE_MISMATCH_IMPORT";
    default:                       return "WASM_ERR_UNKNOWN";
    }
}

/* ---- Section IDs (WASM MVP core, section 5.5.2) ---- */
typedef enum {
    WASM_SECT_CUSTOM   = 0,
    WASM_SECT_TYPE     = 1,
    WASM_SECT_IMPORT   = 2,
    WASM_SECT_FUNCTION = 3,
    WASM_SECT_TABLE    = 4,
    WASM_SECT_MEMORY   = 5,
    WASM_SECT_GLOBAL   = 6,
    WASM_SECT_EXPORT   = 7,
    WASM_SECT_START    = 8,
    WASM_SECT_ELEMENT  = 9,
    WASM_SECT_CODE     = 10,
    WASM_SECT_DATA     = 11,
    WASM_SECT_DATACOUNT= 12,
    WASM_SECT_MAX      = 12
} wasm_section_id_t;

/*
 * Zero-copy byte slice: pointer + length into the original module buffer.
 * Never owns memory; never allocated/freed.
 */
typedef struct {
    const uint8_t *data;
    size_t len;
} wasm_slice_t;

/*
 * Section metadata header (fixed 24 bytes on LP64: 1 id + 3 pad + 4 size + 16 slice).
 * `payload` points directly into the input buffer (zero-copy, no malloc).
 */
typedef struct {
    uint8_t id;            /* wasm_section_id_t */
    uint32_t size;         /* payload length in bytes (ULEB128-decoded) */
    wasm_slice_t payload;  /* payload.data -> section content, len == size */
} wasm_section_header_t;

/* ---- Phase 2: values, stack limits, linear memory, traps ---- */

/* Linear memory page: 64 KiB (WASM MVP core, section 5.4). */
#define WASM_PAGE_SIZE 65536u
#define WASM_PAGE_SIZE_BYTES WASM_PAGE_SIZE
/* Absolute spec cap on pages (used only as a sanity ceiling). */
#define WASM_MEMORY_MAX_PAGES_ABSOLUTE 65536u

/* Fixed-capacity operand stack (embedded-friendly, statically allocatable). */
#define WASM_STACK_MAX_DEPTH 1024u

/* Phase 3: call-stack / frame bounds (all storage pre-allocated, no malloc). */
#define WASM_MAX_CALL_STACK_DEPTH 64u
#define WASM_MAX_LOCALS_PER_FRAME 32u

/* Phase 5: structured-control bounds + function table size. */
#define WASM_MAX_CONTROL_STACK_DEPTH 32u
#define WASM_MAX_FUNCTIONS 32u

/* Phase 2 (control-flow completion): longest br_table label vector the
 * interpreter walks in one step (table labels, excluding the default).
 * Default 256; override with -DWASM_MAX_BR_TABLE_LABELS=<n>.
 * Larger tables are rejected as WASM_TRAP_INVALID_ARGUMENT, never read
 * proportional to an attacker-controlled count. */
#ifndef WASM_MAX_BR_TABLE_LABELS
#define WASM_MAX_BR_TABLE_LABELS 256u
#endif

/* Value types supported by WebAssembly MVP (opcode encoding values). */
typedef enum {
    WASM_TYPE_I32 = 0x7F,
    WASM_TYPE_I64 = 0x7E,
    WASM_TYPE_F32 = 0x7D,
    WASM_TYPE_F64 = 0x7C
} wasm_valtype_t;

/* Tagged union representing one operand-stack slot / local. */
typedef struct {
    wasm_valtype_t type;
    union {
        int32_t  i32;
        int64_t  i64;
        float    f32;
        double   f64;
    } value;
} wasm_val_t;

/* Runtime trap codes. WASM_TRAP_OK (0) means "no trap". */
typedef enum {
    WASM_TRAP_OK = 0,
    WASM_TRAP_NULL,                 /* NULL pointer argument */
    WASM_TRAP_STACK_OVERFLOW,       /* push past WASM_STACK_MAX_DEPTH */
    WASM_TRAP_STACK_UNDERFLOW,      /* pop/peek on empty stack */
    WASM_TRAP_TYPE_MISMATCH,        /* pop_typed: top type != expected */
    WASM_TRAP_MEMORY_OUT_OF_BOUNDS, /* linear-memory access past current size */
    WASM_TRAP_MEMORY_GROW_FAILED,   /* grow would exceed max/capacity */
    WASM_TRAP_INVALID_ARGUMENT,     /* e.g. init with inconsistent sizes */
    WASM_TRAP_CALL_STACK_OVERFLOW,  /* call depth >= WASM_MAX_CALL_STACK_DEPTH */
    WASM_TRAP_CALL_STACK_UNDERFLOW, /* call pop/top on empty call stack */
    WASM_TRAP_INVALID_LOCAL_INDEX,  /* local idx >= frame locals_count */
    WASM_TRAP_INSUFFICIENT_ARGS,    /* fewer operand values than params */
    WASM_TRAP_UNKNOWN_OPCODE,       /* byte is not a supported opcode */
    WASM_TRAP_CODE_OVERRUN,         /* fetch/immediate runs past code_end */
    WASM_TRAP_DIV_BY_ZERO,          /* i32 div/rem with zero divisor */
    WASM_TRAP_INTEGER_OVERFLOW,     /* INT32_MIN / -1 (div_s, rem_s) */
    WASM_TRAP_UNREACHABLE,          /* OP_UNREACHABLE executed */
    WASM_TRAP_CONTROL_STACK_OVERFLOW, /* block nesting >= MAX_CONTROL_DEPTH */
    WASM_TRAP_CONTROL_STACK_UNDERFLOW,/* END with no open block */
    WASM_TRAP_INVALID_BLOCK_LABEL,  /* br label >= open block depth */
    WASM_TRAP_ELSE_WITHOUT_IF,      /* ELSE outside an IF then-branch */
    WASM_TRAP_INVALID_CONVERSION    /* float->int trunc of NaN (out-of-range uses INTEGER_OVERFLOW) */
} wasm_trap_t;

/* Human-readable trap name. Zero-alloc: returns string literals. */
static inline const char *wasm_trap_str(wasm_trap_t t)
{
    switch (t) {
    case WASM_TRAP_OK:                  return "WASM_TRAP_OK";
    case WASM_TRAP_NULL:                return "WASM_TRAP_NULL";
    case WASM_TRAP_STACK_OVERFLOW:      return "WASM_TRAP_STACK_OVERFLOW";
    case WASM_TRAP_STACK_UNDERFLOW:     return "WASM_TRAP_STACK_UNDERFLOW";
    case WASM_TRAP_TYPE_MISMATCH:       return "WASM_TRAP_TYPE_MISMATCH";
    case WASM_TRAP_MEMORY_OUT_OF_BOUNDS:return "WASM_TRAP_MEMORY_OUT_OF_BOUNDS";
    case WASM_TRAP_MEMORY_GROW_FAILED:  return "WASM_TRAP_MEMORY_GROW_FAILED";
    case WASM_TRAP_INVALID_ARGUMENT:    return "WASM_TRAP_INVALID_ARGUMENT";
    case WASM_TRAP_CALL_STACK_OVERFLOW: return "WASM_TRAP_CALL_STACK_OVERFLOW";
    case WASM_TRAP_CALL_STACK_UNDERFLOW:return "WASM_TRAP_CALL_STACK_UNDERFLOW";
    case WASM_TRAP_INVALID_LOCAL_INDEX: return "WASM_TRAP_INVALID_LOCAL_INDEX";
    case WASM_TRAP_INSUFFICIENT_ARGS:   return "WASM_TRAP_INSUFFICIENT_ARGS";
    case WASM_TRAP_UNKNOWN_OPCODE:      return "WASM_TRAP_UNKNOWN_OPCODE";
    case WASM_TRAP_CODE_OVERRUN:        return "WASM_TRAP_CODE_OVERRUN";
    case WASM_TRAP_DIV_BY_ZERO:         return "WASM_TRAP_DIV_BY_ZERO";
    case WASM_TRAP_INTEGER_OVERFLOW:    return "WASM_TRAP_INTEGER_OVERFLOW";
    case WASM_TRAP_UNREACHABLE:         return "WASM_TRAP_UNREACHABLE";
    case WASM_TRAP_CONTROL_STACK_OVERFLOW:  return "WASM_TRAP_CONTROL_STACK_OVERFLOW";
    case WASM_TRAP_CONTROL_STACK_UNDERFLOW: return "WASM_TRAP_CONTROL_STACK_UNDERFLOW";
    case WASM_TRAP_INVALID_BLOCK_LABEL: return "WASM_TRAP_INVALID_BLOCK_LABEL";
    case WASM_TRAP_ELSE_WITHOUT_IF:     return "WASM_TRAP_ELSE_WITHOUT_IF";
    case WASM_TRAP_INVALID_CONVERSION:  return "WASM_TRAP_INVALID_CONVERSION";
    default:                            return "WASM_TRAP_UNKNOWN";
    }
}

/* ---- Phase 6: host function callbacks ----
 * A host function receives its arguments as a borrowed array (popped from
 * the operand stack by the engine), writes up to `nrets` results, and
 * returns a trap code. Zero allocation; never retains the pointers. */
struct wasm_vm; /* defined in wasm_vm.h */
typedef wasm_trap_t (*wasm_host_func_t)(struct wasm_vm *vm,
                                        const wasm_val_t *args, uint32_t nargs,
                                        wasm_val_t *rets, uint32_t nrets);

/* ---- Phase 4: MVP opcodes implemented by the execution core ----
 * Hex values are the exact WebAssembly MVP encodings (section 5.4). */
typedef enum {
    OP_UNREACHABLE = 0x00,
    OP_BLOCK       = 0x02,
    OP_LOOP        = 0x03,
    OP_IF          = 0x04,
    OP_ELSE        = 0x05,
    OP_END         = 0x0B,
    OP_BR          = 0x0C,
    OP_BR_IF       = 0x0D,
    OP_BR_TABLE    = 0x0E,
    OP_RETURN      = 0x0F,
    OP_CALL        = 0x10,
    /* 0x1A (drop) is MVP but not implemented; 0x1B select is. */
    OP_SELECT      = 0x1B,
    OP_LOCAL_GET   = 0x20,
    OP_LOCAL_SET   = 0x21,
    OP_LOCAL_TEE   = 0x22,
    OP_I32_LOAD    = 0x28,
    OP_I32_STORE   = 0x36,
    OP_I32_CONST   = 0x41,
    OP_I64_CONST   = 0x42,
    OP_F32_CONST   = 0x43, /* raw 4-byte LE immediate */
    OP_F64_CONST   = 0x44, /* raw 8-byte LE immediate */
    OP_I32_EQZ     = 0x45,
    OP_I32_EQ      = 0x46,
    OP_I32_NE      = 0x47,
    OP_I32_LT_S    = 0x48,
    OP_I32_LT_U    = 0x49,
    OP_I32_GT_S    = 0x4A,
    OP_I32_GT_U    = 0x4B,
    OP_I32_LE_S    = 0x4C,
    OP_I32_LE_U    = 0x4D,
    OP_I32_GE_S    = 0x4E,
    OP_I32_GE_U    = 0x4F,
    OP_F32_EQ      = 0x5B,
    OP_F32_NE      = 0x5C,
    OP_F32_LT      = 0x5D,
    OP_F32_GT      = 0x5E,
    OP_F32_LE      = 0x5F,
    OP_F32_GE      = 0x60,
    OP_F64_EQ      = 0x61,
    OP_F64_NE      = 0x62,
    OP_F64_LT      = 0x63,
    OP_F64_GT      = 0x64,
    OP_F64_LE      = 0x65,
    OP_F64_GE      = 0x66,
    OP_I32_ADD     = 0x6A,
    OP_I32_SUB     = 0x6B,
    OP_I32_MUL     = 0x6C,
    OP_I32_DIV_S   = 0x6D,
    OP_I32_DIV_U   = 0x6E,
    OP_I32_REM_S   = 0x6F,
    OP_I32_REM_U   = 0x70,
    OP_I32_AND     = 0x71,
    OP_I32_OR      = 0x72,
    OP_I32_XOR     = 0x73,
    OP_I32_SHL     = 0x74,
    OP_I32_SHR_S   = 0x75,
    OP_I32_SHR_U   = 0x76,
    OP_I64_ADD     = 0x7C,
    OP_I64_SUB     = 0x7D,
    OP_I64_MUL     = 0x7E,
    OP_I64_DIV_S   = 0x7F,
    OP_I64_DIV_U   = 0x80,
    OP_I64_REM_S   = 0x81,
    OP_I64_REM_U   = 0x82,
    OP_I64_AND     = 0x83,
    OP_I64_OR      = 0x84,
    OP_I64_XOR     = 0x85,
    OP_I64_SHL     = 0x86,
    OP_I64_SHR_S   = 0x87,
    OP_I64_SHR_U   = 0x88,
    /* ---- Float arithmetic (exact MVP encodings) ---- */
    OP_F32_ABS     = 0x8B,
    OP_F32_NEG     = 0x8C,
    OP_F32_CEIL    = 0x8D,
    OP_F32_FLOOR   = 0x8E,
    OP_F32_TRUNC   = 0x8F,
    OP_F32_NEAREST = 0x90,
    OP_F32_SQRT    = 0x91,
    OP_F32_ADD     = 0x92,
    OP_F32_SUB     = 0x93,
    OP_F32_MUL     = 0x94,
    OP_F32_DIV     = 0x95,
    OP_F32_MIN     = 0x96,
    OP_F32_MAX     = 0x97,
    OP_F32_COPYSIGN = 0x98,
    OP_F64_ABS     = 0x99,
    OP_F64_NEG     = 0x9A,
    OP_F64_CEIL    = 0x9B,
    OP_F64_FLOOR   = 0x9C,
    OP_F64_TRUNC   = 0x9D,
    OP_F64_NEAREST = 0x9E,
    OP_F64_SQRT    = 0x9F,
    OP_F64_ADD     = 0xA0,
    OP_F64_SUB     = 0xA1,
    OP_F64_MUL     = 0xA2,
    OP_F64_DIV     = 0xA3,
    OP_F64_MIN     = 0xA4,
    OP_F64_MAX     = 0xA5,
    OP_F64_COPYSIGN = 0xA6,
    /* 0xA7 is unused in MVP. */
    OP_I32_TRUNC_F32_S = 0xA8,
    OP_I32_TRUNC_F32_U = 0xA9,
    OP_I32_TRUNC_F64_S = 0xAA,
    OP_I32_TRUNC_F64_U = 0xAB,
    /* 0xAC/0xAD are saturating conversions (post-MVP, not implemented). */
    OP_I64_TRUNC_F32_S = 0xAE,
    OP_I64_TRUNC_F32_U = 0xAF,
    OP_I64_TRUNC_F64_S = 0xB0,
    OP_I64_TRUNC_F64_U = 0xB1,
    OP_F32_CONVERT_I32_S = 0xB2,
    OP_F32_CONVERT_I32_U = 0xB3,
    OP_F32_CONVERT_I64_S = 0xB4,
    OP_F32_CONVERT_I64_U = 0xB5,
    OP_F32_DEMOTE_F64  = 0xB6,
    OP_F64_CONVERT_I32_S = 0xB7,
    OP_F64_CONVERT_I32_U = 0xB8,
    OP_F64_CONVERT_I64_S = 0xB9,
    OP_F64_CONVERT_I64_U = 0xBA,
    OP_F64_PROMOTE_F32 = 0xBB,
    OP_I32_REINTERPRET_F32 = 0xBC,
    OP_I64_REINTERPRET_F64 = 0xBD,
    OP_F32_REINTERPRET_I32 = 0xBE,
    OP_F64_REINTERPRET_I64 = 0xBF
} wasm_opcode_t;

/* ---- Phase 5: structured control-flow block kinds ----
 * Relative branch labels count outward: 0 = innermost open block. */
typedef enum {
    WASM_BLOCK_TYPE_BLOCK = 0,
    WASM_BLOCK_TYPE_LOOP = 1,
    WASM_BLOCK_TYPE_IF = 2
} wasm_block_kind_t;

/* Block result type encoding: -64 (0x40) = void; otherwise an MVP valtype
 * byte (this engine executes i32-valued blocks; others are rejected). */
#define WASM_BLOCKTYPE_VOID (-64)

#endif /* WASM_TYPES_H */
