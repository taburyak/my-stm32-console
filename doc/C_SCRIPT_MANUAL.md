# C-Script (PicoC Subset) User Manual — `my-stm32-console`

This guide explains how to write, deploy, and debug standalone C scripts (`*.c`) that run directly on the **STM32F407 Console** without requiring a PC compiler or toolchain.

Related documents: [../README.md](../README.md), [../sdk_apps/star_catcher.c](../sdk_apps/star_catcher.c), [../sdk_apps/console_sdk.h](../sdk_apps/console_sdk.h).

---

## 1. Overview & How It Works

The console embeds a sandboxed C interpreter ([Core/Src/script_engine.c](../Core/Src/script_engine.c)) in internal MCU Flash:
- **Zero compilation step**: Copy any `.c` text file to `/games/<name>.c` on the MicroSD card. It immediately appears as a cyan tile in the **Games Hub** and can also be launched from the **SD File Explorer**.
- **Hardware-enforced MPU sandbox**: The script runs inside the unprivileged `SandboxTask` (`CONTROL.nPRIV = 1`). All hardware access goes through the exact same `SVC` system calls and syscall gate as compiled `.bin` SDK apps.
- **Fast 0-WS execution**:
  - The interpreter state (`ScriptVM_t`: token stream, symbol tables, variables, call stack) resides in the 28 KB 0-Wait-State internal SRAM sandbox (`0x20018000..0x2001EFFF`), eliminating FSMC bus contention during LCD rendering.
  - Script source text (`0x68080000`, up to 64 KB) and user arrays (`0x680D0000`, up to 192 KB) reside in external FSMC SRAM.

---

## 2. Deploying & Running a Script

1. Create a file named `my_game.c` in any text editor.
2. Copy `my_game.c` to `/games/my_game.c` on the MicroSD card.
3. Optional: Place a 64×64 24-bit or 16-bit BMP icon at `/games/my_game.bmp` — the Games Hub will automatically display it on the tile.
4. Open **Games** on the console and click **`my_game.c`** (or open **Files** → `/games/my_game.c`).
5. To exit any running script back to the OS UI:
   - Press **`ESC`** / **`Backspace`** on USB keyboard, **Button `B`** / **`Select`** on USB gamepad, or **KEY3** on the board (if the script checks `KEY_EXIT`);
   - Or hold **KEY3** (or **KEY0** depending on board silk-screen) for 1 second for a forced OS kill.

---

## 3. Supported C Language Subset

### 3.1 Entry Point
Every script must define either `void main()` or `void app_main()`:
```c
void main() {
    clear_screen(COLOR_DARK_BG);
    draw_text(40, 110, "HELLO FROM C-SCRIPT!", COLOR_CYAN, COLOR_DARK_BG, 2);
    while (1) {
        if (input_poll() & KEY_EXIT) break;
        frame_sync(16);
    }
}
```

### 3.2 Preprocessor & Comments
- Single-line `// ...` and multi-line `/* ... */` comments are supported.
- `#include <...>` and `#include "..."` lines are safely ignored (all SDK functions and constants are built-in).
- `#define NAME VALUE` defines integer or hex constants:
  ```c
  #define MAX_ENEMIES 12
  #define BG_COLOR    0x0862
  ```

### 3.3 Data Types, Variables & Arrays
All scalar variables are stored as signed 32-bit integers (`int32_t`), while arrays respect element width (`1` byte for `char`/`uint8_t`/`bool`, `4` bytes for `int`/`uint32_t`):
- Supported type keywords: `int`, `int32_t`, `int16_t`, `int8_t`, `uint32_t`, `uint16_t`, `uint8_t`, `unsigned`, `char`, `bool`, `void`, `const`, `static`.
- **Global & local scalars**:
  ```c
  int score = 0, lives = 3;
  ```
- **1D Arrays** (allocated from the 192 KB external SRAM arena, initialized with `{...}` or string literals):
  ```c
  int enemy_x[16];
  int palette[4] = { COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW };
  char msg[32]   = "LEVEL 1";
  ```

### 3.4 Operators & Control Flow
- **Arithmetic**: `+`, `-`, `*`, `/`, `%`, unary `+`/`-`, prefix and postfix `++` / `--`.
- **Bitwise**: `&`, `|`, `^`, `~`, `<<`, `>>`.
- **Logical & relational**: `&&`, `||`, `!`, `==`, `!=`, `<`, `<=`, `>`, `>=`.
- **Ternary**: `cond ? a : b`.
- **Compound assignments**: `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`.
- **Control statements**:
  - `if (...) { ... } else if (...) { ... } else { ... }`
  - `while (...) { ... }`
  - `do { ... } while (...);`
  - `for (int i = 0; i < N; i++) { ... }`
  - `break;`, `continue;`, `return;`, `return expr;`
- **User functions** (up to 32 functions, up to 8 parameters each, recursion supported up to depth 24):
  ```c
  int clamp_x(int x, int w) {
      if (x < 0) return 0;
      if (x + w > SCREEN_W) return SCREEN_W - w;
      return x;
  }
  ```

### 3.5 Resource Limits
| Resource | Limit | Location |
|----------|-------|----------|
| Script source file size | 64 KB | `0x68080000` (External SRAM) |
| Tokens (`MAX_TOKENS`) | 2048 | `0x20018000` (Internal 0-WS SRAM) |
| Unique identifiers (`MAX_IDENTS`) | 224 (up to 19 chars) | `0x20018000` (Internal 0-WS SRAM) |
| Global variables (`MAX_GLOBALS`) | 128 | `0x20018000` (Internal 0-WS SRAM) |
| Active local variables (`MAX_LOCALS`) | 64 | `0x20018000` (Internal 0-WS SRAM) |
| User functions (`MAX_FUNCS`) | 32 | `0x20018000` (Internal 0-WS SRAM) |
| User array arena | 192 KB | `0x680D0000` (External SRAM) |

If a syntax or runtime error occurs (e.g. undefined variable, array index out of bounds, missing semicolon), the engine displays a **`C-SCRIPT ERROR`** screen with the exact line number (`LINE: N`) and message. Press `ESC`, `A`, or `B` to return to the Games Hub.

---

## 4. Built-in SDK API Reference

### 4.1 Graphics & Display (320×240 RGB565)
| Function | Description |
|----------|-------------|
| `clear_screen(color)` | Clears full 320×240 display with `color` (uses hardware DMA2 constant fill). |
| `fill_rect(x, y, w, h, color)` | Fills rectangle `[x..x+w-1, y..y+h-1]` with `color` (DMA2 fill for ≥ 256 px). |
| `draw_pixel(x, y, color)` | Draws a single pixel at `(x, y)`. |
| `draw_text(x, y, str, fg, [bg], [scale])` | Draws string `str` using the kernel 5×7 font. `bg` defaults to `COLOR_BLACK`, `scale` defaults to `1` (6×7 px per char). |
| `draw_num(x, y, val, fg, [bg], [scale], [min_digits])` | Formats signed integer `val` (zero-padded to `min_digits`) and draws it at `(x, y)`. |
| `RGB565(r, g, b)` | Packs 8-bit `r, g, b` (`0..255`) into a 16-bit RGB565 color value. |

**Tips for flicker-free rendering:**
- Do **not** call `clear_screen()` inside your main loop. Clear once before the loop, then erase only moving objects at their old coordinates right before drawing them at their new coordinates:
  ```c
  fill_rect(old_x, old_y, w, h, COLOR_DARK_BG);
  fill_rect(new_x, new_y, w, h, COLOR_AMBER);
  ```
- Redraw the score HUD only when the score actually changes.

### 4.2 Input (Board Buttons, USB Gamepad, USB Keyboard, Mouse & Touch)
| Function | Description |
|----------|-------------|
| `input_poll()` | Samples all inputs, updates internal edge/auto-repeat state, and returns the held `KEY_*` bitmask. Call once per frame! |
| `input_pressed(mask)` | Non-zero on the single frame a key in `mask` transitions from released to pressed. |
| `input_held(mask)` | Non-zero while any key in `mask` is held down. |
| `input_repeat(mask)` | Non-zero on initial press and at DAS (170 ms) / ARR (50 ms) auto-repeat intervals. |
| `get_keys()` | Raw instant `KEY_*` bitmask without updating edge state. |
| `pad_lx()`, `pad_ly()` | Left analog stick X / Y axes (`-127 .. +127`, `0` when centered or disconnected). |
| `pad_rx()`, `pad_ry()` | Right analog stick X / Y axes (`-127 .. +127`). |
| `pad_buttons()` | Full gamepad button mask (`SDK_PAD_*`, including `L1/R1/L2/R2/Start/Select/L3/R3`). |
| `mouse_x()`, `mouse_y()` | USB mouse or XPT2046 touch coordinates (`0..319`, `0..239`, or `-1` if inactive). |
| `mouse_buttons()` | Mouse/touch button mask (`1` = left click / touch press, `2` = right, `4` = middle). |

### 4.3 Timing, Audio, Random & Math
| Function | Description |
|----------|-------------|
| `frame_sync(period_ms)` | Sleeps until `period_ms` has elapsed since the previous frame (`16` ms ≈ 60 FPS). Yields CPU to RTOS tasks and refreshes watchdog. |
| `sleep_ms(ms)` / `delay_ms(ms)` | Pauses the script task for `ms` milliseconds via the kernel gate. |
| `get_tick()` | System uptime in milliseconds (`HAL_GetTick()`). |
| `beep(ms)` | Sounds the board buzzer for `ms` milliseconds (non-blocking). |
| `play_tone(freq_hz, ms)` | Plays a tone for `ms` milliseconds. |
| `led_toggle(id)` | Toggles status LED (`1` = disk LED). |
| `rand([max])` | Hardware RNG random number (`0..max-1` if `max > 0`, otherwise `0..0x7FFFFFFF`). |
| `rand_range(min, max)` | Uniform random integer in `[min, max - 1]`. |
| `abs(x)`, `min(a, b)`, `max(a, b)`, `clamp(v, lo, hi)` | Fast built-in math helpers. |
| `exit()` | Immediately terminates the script and returns to the Games Hub. |

### 4.4 SD Card Files & Non-Volatile Storage (Saves & Assets)

Every script automatically gets its own isolated directory on the SD card based on its filename (for example, `/games/star_catcher.c` maps to `SD:/appdata/star_catcher/` for read/write saves and `SD:/games/star_catcher/` via the `"res:"` prefix for read-only assets). All open file handles are automatically closed by the OS if the script exits.

#### High-Level One-Line Save/Load Helpers (SD Card):
| Function | Description |
|----------|-------------|
| `save_int("best.dat", value)` | Writes a 32-bit integer `value` to `SD:/appdata/<script_name>/best.dat` (creates directory and file automatically). Returns `1` on success, `0` on error. |
| `load_int("best.dat", default_val)` | Reads a 32-bit integer from `SD:/appdata/<script_name>/best.dat`, or returns `default_val` if the file does not exist yet. |

#### Low-Level SD Card File API:
| Function | Description |
|----------|-------------|
| `file_open(path, flags)` | Opens a file in `SD:/appdata/<script_name>/` (or `"res:file.bin"` in `SD:/games/<script_name>/`). `flags`: combination of `SDK_FILE_READ`, `SDK_FILE_WRITE`, `SDK_FILE_CREATE`, `SDK_FILE_TRUNC`, `SDK_FILE_APPEND`. Returns descriptor `fd >= 0` or negative `SDK_ERR_*`. |
| `file_read(fd, array_buf, len)` | Reads up to `len` bytes into a script array (`char buf[N]` or `int data[N]`). Returns bytes read (`>= 0`) or negative error. |
| `file_write(fd, array_buf, len)` | Writes `len` bytes from a script array or string to `fd`. Returns bytes written. |
| `file_seek(fd, offset)` | Moves file pointer to byte `offset` from the start of the file. |
| `file_size(fd)` | Returns total file size in bytes. |
| `file_close(fd)` | Closes file descriptor `fd`. |
| `file_remove(path)` | Deletes a file in the script's `appdata` directory. |

#### EEPROM Window (128 bytes at `0x80..0xFF`, works without SD card):
| Function | Description |
|----------|-------------|
| `eeprom_read_u8(offset)` | Reads 1 byte at `offset` (`0..127`) from AT24C02 EEPROM (or `-1` on error). |
| `eeprom_write_u8(offset, val)` | Writes 1 byte (`0..255`) at `offset` (`0..127`) to EEPROM (skips write if unchanged). |

---

## 5. Built-in Constants

### Key Masks (`input_poll()`, `input_pressed()`, `get_keys()`)
- `KEY_LEFT`, `KEY_RIGHT`, `KEY_UP`, `KEY_DOWN` (D-Pad, Left Analog Stick, Keyboard Arrows / WASD, Board Keys)
- `KEY_A` (Gamepad A / Space / Enter)
- `KEY_B` (Gamepad X / Z / X)
- `KEY_X` (Gamepad Y / C)
- `KEY_START` (Gamepad Start / L2 / R2 / Keyboard P)
- `KEY_EXIT` (Gamepad B / Select / Keyboard ESC / Backspace / Board KEY3)

### Gamepad Buttons (`pad_buttons()`)
- `SDK_PAD_UP`, `SDK_PAD_DOWN`, `SDK_PAD_LEFT`, `SDK_PAD_RIGHT`
- `SDK_PAD_A`, `SDK_PAD_B`, `SDK_PAD_X`, `SDK_PAD_Y`
- `SDK_PAD_L1`, `SDK_PAD_R1`, `SDK_PAD_L2`, `SDK_PAD_R2`
- `SDK_PAD_START`, `SDK_PAD_SELECT`, `SDK_PAD_L3`, `SDK_PAD_R3`

### Colors & Screen Dimensions
- `SCREEN_W` (`320`), `SCREEN_H` (`240`)
- `COLOR_BLACK` (`0x0000`), `COLOR_WHITE` (`0xFFFF`), `COLOR_CYAN` (`0x07FF`), `COLOR_NEON_GREEN` / `COLOR_GREEN` (`0x07E0`), `COLOR_AMBER` / `COLOR_ORANGE` (`0xFD20`), `COLOR_RED` (`0xF800`), `COLOR_BLUE` (`0x001F`), `COLOR_MAGENTA` (`0xF81F`), `COLOR_YELLOW` (`0xFFE0`), `COLOR_DARK_BG` (`0x0862`), `COLOR_PANEL_BG` (`0x10A4`), `COLOR_GRID_LINE` (`0x1926`).

---

## 6. Complete Example: Saving High Score & Reading/Writing SD Files

```c
// Save & Load High Score + Custom Array Log on SD Card
int history[10];

void main() {
    int best  = load_int("best.dat", 0);
    int score = 0;

    // Load last 10 scores from SD:/appdata/<script_name>/scores.bin
    int fd = file_open("scores.bin", SDK_FILE_READ);
    if (fd >= 0) {
        file_read(fd, history, 40); // 10 ints * 4 bytes
        file_close(fd);
    }

    clear_screen(COLOR_DARK_BG);
    draw_text(10, 10, "PRESS A: +1   B: EXIT", COLOR_CYAN, COLOR_DARK_BG, 1);

    while (1) {
        input_poll();
        if (input_pressed(KEY_EXIT)) break;

        if (input_pressed(KEY_A)) {
            score++;
            beep(20);
            if (score > best) {
                best = score;
                save_int("best.dat", best);
            }
        }

        draw_text(10, 40, "SCORE:", COLOR_WHITE, COLOR_DARK_BG, 2);
        draw_num(90, 40, score, COLOR_NEON_GREEN, COLOR_DARK_BG, 2, 3);

        draw_text(10, 70, "BEST:", COLOR_WHITE, COLOR_DARK_BG, 2);
        draw_num(90, 70, best, COLOR_AMBER, COLOR_DARK_BG, 2, 3);

        frame_sync(16);
    }

    // Shift history and save on exit
    for (int i = 9; i > 0; i--) {
        history[i] = history[i - 1];
    }
    history[0] = score;

    fd = file_open("scores.bin", SDK_FILE_WRITE | SDK_FILE_CREATE | SDK_FILE_TRUNC);
    if (fd >= 0) {
        file_write(fd, history, 40);
        file_close(fd);
    }
}
```
