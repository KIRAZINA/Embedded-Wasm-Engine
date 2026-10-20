/*
 * realworld/crc32_demo.c — readiness program 3: CRC32 over a fixed buffer.
 *
 * Real clang output. Computes the standard CRC-32 (IEEE 802.3, reflected,
 * polynomial 0xEDB88320) of a fixed message using the classic 256-entry
 * lookup table. The table lives in the module's data section (1 KiB),
 * the message is a data segment as well.
 *
 * Exercises bit operations (xor, shift, and), table-driven memory
 * access with a computed index, and a loop over a byte buffer.
 */

#if defined(NATIVE_BUILD)
#include <stdio.h>
#endif

#include <stdint.h>

#if defined(__wasm__)
#define EXPORT_MAIN __attribute__((export_name("main")))
#else
#define EXPORT_MAIN
#endif

#include "crc32_table.h"

static volatile unsigned char kMessage[] = {
    'W', 'a', 's', 'm', ' ', 'M', 'V', 'P', ' ', 'r', 'e', 'a', 'd',
    'y', ' ', 'f', 'o', 'r', ' ', 'r', 'e', 'a', 'l', ' ', 'c', 'o',
    'd', 'e', ' ', '!', ' ', 'P', 'h', 'a', 's', 'e', ' ', '5', '.',
    '\r', '\n', 'e', 'n', 'd'
};

#define MESSAGE_LEN ((uint32_t)(sizeof(kMessage) / sizeof(kMessage[0])))

static unsigned int compute(void)
{
    unsigned int crc = 0xFFFFFFFFu;
    unsigned int i;

    for (i = 0u; i < MESSAGE_LEN; i++) {
        unsigned int index = (crc ^ (unsigned int)kMessage[i]) & 0xFFu;
        crc = kCrcTable[index] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

#if defined(NATIVE_BUILD)
int main(void)
{
    printf("result: %u (i32)\n", compute());
    return 0;
}
#else
EXPORT_MAIN int wasm_entry(void)
{
    return (int)compute();
}
#endif
