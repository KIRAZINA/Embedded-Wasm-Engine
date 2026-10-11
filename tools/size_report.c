#include <stdio.h>

#include "wasm_host.h"
#include "wasm_memory.h"
#include "wasm_vm.h"

#ifndef STM32F4_VM_BUDGET_BYTES
#define STM32F4_VM_BUDGET_BYTES (112u * 1024u) /* maximum VM bytes on STM32F4 */
#endif

_Static_assert(sizeof(wasm_vm_t) <= STM32F4_VM_BUDGET_BYTES,
               "wasm_vm_t exceeds documented STM32F4 VM budget");

int main(void)
{
    size_t vm = sizeof(wasm_vm_t);
    size_t registry = sizeof(wasm_host_registry_t);
    size_t memory = sizeof(wasm_memory_t);
    printf("wasm_vm_t=%lu bytes\n", (unsigned long)vm);
    printf("wasm_host_registry_t=%lu bytes\n", (unsigned long)registry);
    printf("wasm_memory_t=%lu bytes\n", (unsigned long)memory);
    printf("engine_static_footprint=%lu bytes\n",
           (unsigned long)(vm + registry + memory));
    return 0;
}
