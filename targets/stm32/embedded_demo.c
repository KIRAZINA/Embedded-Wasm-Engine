#include <stddef.h>
#include <stdint.h>

#include "wasm_host.h"
#include "wasm_memory.h"
#include "wasm_parser.h"
#include "wasm_validate.h"
#include "wasm_vm.h"

extern const uint8_t embedded_demo_wasm[];
extern const size_t embedded_demo_wasm_len;

/* Board support replaces this function with ITM, semihosting, UART, etc. */
void embedded_putc(char c)
{
    (void)c;
}

static void print_i32(int32_t value)
{
    char digits[12];
    uint32_t magnitude;
    unsigned count = 0u;
    if (value < 0) {
        embedded_putc('-');
        magnitude = (uint32_t)(-(value + 1)) + 1u;
    } else {
        magnitude = (uint32_t)value;
    }
    do {
        digits[count++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u);
    while (count > 0u) embedded_putc(digits[--count]);
    embedded_putc('\n');
}

int main(void)
{
    static uint8_t memory_bytes[WASM_PAGE_SIZE];
    static wasm_vm_t vm;
    static wasm_memory_t memory;
    static wasm_host_registry_t registry;
    wasm_module_t module;
    wasm_section_header_t export_section;
    wasm_slice_t export_payload;
    wasm_val_t value;
    uint32_t function_index;
    wasm_err_t error;
    wasm_trap_t trap;
    const wasm_func_desc_t *function;

    error = wasm_parse_header(embedded_demo_wasm, embedded_demo_wasm_len, &module);
    if (error != WASM_OK) return (int)error;
    error = wasm_validate_module(&module);
    if (error != WASM_OK) return (int)error;
    if (wasm_vm_init(&vm) != WASM_TRAP_OK ||
        wasm_host_registry_init(&registry) != WASM_TRAP_OK ||
        wasm_memory_init(&memory, memory_bytes, (uint32_t)sizeof(memory_bytes),
                         0u, 1u) != WASM_TRAP_OK ||
        wasm_vm_bind_memory(&vm, &memory) != WASM_TRAP_OK) return 1;
    error = wasm_instantiate(&vm, &module, &registry, NULL);
    if (error != WASM_OK) return (int)error;
    error = wasm_find_section(&module, (uint8_t)WASM_SECT_EXPORT, &export_section);
    if (error != WASM_OK) return (int)error;
    export_payload = export_section.payload;
    error = wasm_get_export_func_index(&export_payload, "main", &function_index);
    if (error != WASM_OK || function_index >= WASM_MAX_FUNCTIONS) return 1;
    function = &vm.funcs[function_index];
    if (function->param_count != 2u || function->return_count != 1u) return 1;
    {
        wasm_val_t first = { WASM_TYPE_I32, { .i32 = 3 } };
        wasm_val_t second = { WASM_TYPE_I32, { .i32 = 4 } };
        if (wasm_stack_push(&vm.operands, first) != WASM_TRAP_OK ||
            wasm_stack_push(&vm.operands, second) != WASM_TRAP_OK) return 1;
    }
    trap = wasm_vm_enter_function(&vm, function_index, 2u,
                                  function->local_count, function->return_count);
    if (trap != WASM_TRAP_OK) return (int)trap;
    trap = wasm_vm_attach_code(&vm, function->code, function->code_len);
    if (trap != WASM_TRAP_OK) return (int)trap;
    while (wasm_call_stack_depth(&vm.calls) > 0u) {
        wasm_frame_t *frame = NULL;
        if (wasm_call_stack_top(&vm.calls, &frame) != WASM_TRAP_OK) return 1;
        if (frame->code_ptr >= frame->code_end) {
            trap = wasm_vm_exit_function(&vm, frame->return_count);
        } else {
            trap = wasm_vm_step(&vm);
        }
        if (trap != WASM_TRAP_OK) return (int)trap;
    }
    trap = wasm_stack_pop_typed(&vm.operands, WASM_TYPE_I32, &value);
    if (trap != WASM_TRAP_OK) return (int)trap;
    print_i32(value.value.i32);
    return 0;
}
