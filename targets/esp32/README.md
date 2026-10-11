# ESP32 target

The engine is a plain C component and uses no heap APIs. With ESP-IDF 5.x:

```sh
idf.py create-project wasm_engine_demo
cd wasm_engine_demo
mkdir -p components/wasm_engine
cp -r /path/to/WebAssembly\ Engine/include components/wasm_engine/
cp -r /path/to/WebAssembly\ Engine/src components/wasm_engine/
cp /path/to/WebAssembly\ Engine/targets/esp32/CMakeLists.txt components/wasm_engine/
idf.py set-target esp32        # use esp32c3 for ESP32-C3
idf.py menuconfig
idf.py build
idf.py flash monitor
```

The application supplies its own static memory arena and registers any board
I/O through `wasm_host_registry_t`. No Xtensa/RISC-V ESP-IDF build is run by
the repository CI; the exact IDF version and board configuration remain an
integration responsibility of the application.
