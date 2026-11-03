#ifndef SCRIPT_ENGINE_H
#define SCRIPT_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Sandbox C-Script Engine (PicoC subset in MCU Flash).
 *
 * Memory layout:
 *   - Internal SRAM App Sandbox (`0x20018000 .. 0x2001EFFF`, 28 KB, 0-WS 32-bit internal SRAM):
 *     holds the active `ScriptVM_t` runtime state (scalars, call stack, PC, symbol tables).
 *     Because C-scripts do not load a `.bin` image into `0x20018000`, all 28 KB of fast
 *     internal sandbox RAM are free and completely isolated from FSMC LCD traffic!
 *   - External SRAM App Sandbox (`0x68080000 .. 0x680FFFFF`, 512 KB):
 *     * `0x68080000 .. 0x6808FFFF` (64 KB)  : Script source code buffer
 *     * `0x68090000 .. 0x6809FFFF` (64 KB)  : Token stream (`Token_t[MAX_TOKENS]`)
 *     * `0x680D0000 .. 0x680FFFFF` (192 KB) : Script user arrays & data buffers
 */
#define SCRIPT_VM_INTERNAL_ADDR   0x20018000UL
#define SCRIPT_VM_INTERNAL_MAX    (28UL * 1024UL)

#define SCRIPT_SOURCE_BASE_ADDR   0x68080000UL
#define SCRIPT_SOURCE_MAX_SIZE    (64UL * 1024UL)
#define SCRIPT_TOKENS_BASE_ADDR   0x68090000UL
#define SCRIPT_TOKENS_MAX_SIZE    (64UL * 1024UL)
#define SCRIPT_ARRAY_BASE_ADDR    0x680D0000UL
#define SCRIPT_ARRAY_MAX_SIZE     (192UL * 1024UL)

/**
 * @brief Entry point invoked inside the unprivileged SandboxTask to parse and execute
 *        the C-script stored at SCRIPT_SOURCE_BASE_ADDR.
 */
void ScriptEngine_RunSandbox(void);

#ifdef __cplusplus
}
#endif

#endif /* SCRIPT_ENGINE_H */
