idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../src/leb128.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../src/wasm_parser.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../src/wasm_stack.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../src/wasm_memory.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../src/wasm_frame.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../src/wasm_control.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../src/wasm_vm.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../src/wasm_host.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../src/wasm_validate.c"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../include"
    COMPILE_DEFINITIONS
        # Embedded overrides: the ESP32 build never links the desktop CLI,
        # but the file budget stays explicit for every target.
        RUNNER_FILE_MAX=4096u
        STM32F4_VM_BUDGET_BYTES=114688u
)
