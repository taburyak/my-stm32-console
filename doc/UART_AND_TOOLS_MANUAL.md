# UART Uploader, Asset Converter & Dev Tools Manual — `my-stm32-console`

This manual covers the PC-side tools (`tools/`) and on-device `USART3` transfer engine (`Core/Src/uart_loader.c`) used to upload games, C-scripts, icons, and assets, inspect or clean up the MicroSD card, pull diagnostic logs, and check memory budgets without removing the SD card from the console.

Related files: [../tools/uart_upload.py](../tools/uart_upload.py), [../tools/img2rgb565.py](../tools/img2rgb565.py), [../tools/map_budget.py](../tools/map_budget.py), [../Core/Src/uart_loader.c](../Core/Src/uart_loader.c), [C_SCRIPT_MANUAL.md](C_SCRIPT_MANUAL.md).

---

## 1. Prerequisites & Hardware Setup

- **UART Connection**: `USART3` (`PB10` = TX, `PB11` = RX) connected to a USB-UART bridge (e.g., `Silicon Labs CP210x`, `CH340`, `FTDI`) at **`115200 8N1`**.
- **Python dependency** (for `uart_upload.py`):
  ```powershell
  pip install pyserial
  ```
  Optional (for resizing JPG/complex PNGs in `img2rgb565.py`; standard BMP and 8-bit PNG work out of the box without dependencies):
  ```powershell
  pip install Pillow
  ```
- Make sure no other terminal program (PuTTY, Tera Term, VS Code Serial Monitor) is holding the COM port open before running `uart_upload.py`.

---

## 2. Zero-Touch UART Upload & SD Management (`tools/uart_upload.py`)

### 2.1 How Zero-Touch Mode Works
You **do not need to press any buttons** on the console:
1. When `tools/uart_upload.py` starts, it sends the 6-byte wake sequence `"UPLD!\n"` over `USART3`.
2. `USART3_IRQHandler` detects the sequence in the background. If a game or C-script is currently running, the OS automatically exits the sandbox, opens the **`USART3 UPLOADER`** screen, and replies with `"RDY\n"`.
3. The script executes all requested operations (listing, deleting, pulling, or uploading multiple files) in **a single serial session** with IEEE `CRC-32` verification.

---

### 2.2 Common Commands

*(If `--port COMx` is omitted, `uart_upload.py` auto-detects the connected USB-UART bridge.)*

#### Upload & immediately run a compiled `.bin` game (also saves to `SD:/games/<name>.bin`)
```powershell
python tools/uart_upload.py build/Debug/sdk_apps/stack_up.bin --port COM9 --run
```

#### Upload & immediately run a `.c` C-Script (also saves to `SD:/games/<name>.c`)
```powershell
python tools/uart_upload.py sdk_apps/star_catcher.c --port COM9 --run
```

#### Test in RAM only (do not write to the MicroSD card)
```powershell
python tools/uart_upload.py build/Debug/sdk_apps/stack_up.bin --port COM9 --ram-only
```

#### Upload to SD card without launching (`--no-run`)
```powershell
python tools/uart_upload.py build/Debug/sdk_apps/stack_up.bin --port COM9 --no-run
```

#### Batch upload: Game Icon (`.bmp`) + Game Binary (`.bin`) + Auto-Run in one session
Passing multiple files sends all of them over a single COM port connection, saves each to `SD:/games/`, and launches the last `.bin`/`.c` file:
```powershell
python tools/uart_upload.py sdk_apps/stack_up.bmp build/Debug/sdk_apps/stack_up.bin --port COM9 --run
```

#### Upload a file to a custom SD card path (`--dest`)
If `--dest` has no `/` separator, the file is placed in `SD:/games/`. To place a file in another folder (such as `assets/` or `system/`), include the folder path:
```powershell
python tools/uart_upload.py my_wall.bmp --dest assets/wall.bmp --port COM9 --no-run
```

---

### 2.3 Inspecting, Pulling, and Deleting Files on the SD Card

#### List the entire SD card directory tree (`--ls`) or a specific folder (`--ls-dir`)
```powershell
# Full 2-level tree of /, /games, /assets, /appdata, /system with file sizes:
python tools/uart_upload.py --ls --port COM9

# List only a specific directory:
python tools/uart_upload.py --ls-dir games --port COM9
python tools/uart_upload.py --ls-dir appdata/sram_test --port COM9
```

#### Pull (download) a log or save file from the SD card to PC (`--pull`)
```powershell
# Print SD:/appdata/sram_test/report.txt directly to the terminal:
python tools/uart_upload.py --pull appdata/sram_test/report.txt --port COM9

# Save to a local file on the PC:
python tools/uart_upload.py --pull appdata/sram_test/report.txt -o sram_report.txt --port COM9
```

#### Delete one or more files from the SD card (`--delete`)
If no folder prefix is specified, `--delete` targets `SD:/games/<name>`:
```powershell
# Delete SD:/games/old_game.bin:
python tools/uart_upload.py --delete old_game.bin --port COM9

# Delete multiple files in one session:
python tools/uart_upload.py --delete old_game.bin --delete old_game.bmp --port COM9
```

---

### 2.4 Manual XMODEM-CRC Mode (`UART Rx` Tile in Games Hub)
If you are using a classic serial terminal (**Tera Term**, **ExtraPuTTY**, **minicom**) without Python:
1. Open **Games** on the console and click the **`UART Rx`** tile.
2. The console displays `WAITING FOR PC (ESC: CANCEL)` and polls with `'C'` (`0x43`) every second for **XMODEM-CRC / XMODEM-1K**.
3. In Tera Term, choose **`File -> Transfer -> XMODEM -> Send...`** (select `CRC` or `1K`) and pick your `.bin` or `.c` file.
4. When transfer completes:
   - If the file has an `SDK_AppHeader_t` (`"CAPP"` magic), the console extracts the exact `image_size` (stripping XMODEM `0x1A` padding) and `hdr->name`, saves it as `SD:/games/<hdr.name>.bin`, and launches it.
   - Otherwise, trailing `0x1A`/`0x00` padding is stripped and the file is saved and executed as `SD:/games/uart_app.c`.

*(You can also force `uart_upload.py` to use standard XMODEM-1K by passing `--xmodem`.)*

---

## 3. Image & Icon Converter (`tools/img2rgb565.py`)

Converts PNG, BMP, or JPG images into RGB565 formats ready for the console UI or SDK games.

| Flag / Option | Description |
|---|---|
| `-o <path>` | Output file (`.h`, `.raw`, `.simg`, or `.bmp`). Format is auto-detected from extension. |
| `-f {c,raw,simg,bmp16}` | Explicit output format override. |
| `--icon` | Resize to **`64x64`** (for Games Hub tile icons `SD:/games/<app>.bmp`). |
| `--wallpaper` | Resize to **`320x216`** (for Home screen workspace wallpapers). |
| `--resize WxH` | Resize to arbitrary dimensions (e.g., `--resize 128x64`). |
| `--dither` | Apply 2×2 ordered Bayer dithering before RGB565 quantization (smooths gradients). |
| `-n <symbol>` | C array identifier name when generating a `.h` header. |
| `--alpha-key 0x0000` | RGB565 color written for transparent pixels (`alpha < --alpha-threshold`). |

### Examples

1. **Create a 64×64 Games Hub tile icon (`bmp16`) and upload it over UART**:
   ```powershell
   python tools/img2rgb565.py my_art.png -o sdk_apps/stack_up.bmp --icon --dither
   python tools/uart_upload.py sdk_apps/stack_up.bmp --port COM9 --no-run
   ```
2. **Generate a C sprite header (`const uint16_t spr_hero[W * H]`) for `sys_draw_sprite` / `sys_lcd_blit`**:
   ```powershell
   python tools/img2rgb565.py hero.png -o sdk_apps/hero_spr.h --name spr_hero --resize 16x16
   ```
3. **Generate a raw RGB565 binary asset (`.raw`) to load from SD (`res:`) into `.ext_bss`**:
   ```powershell
   python tools/img2rgb565.py tileset.png -o tileset.raw --format raw
   ```

---

## 4. Linker Memory Budget Analyzer (`tools/map_budget.py`)

Parses `build/Debug/my-stm32-console.map` and all `build/Debug/sdk_apps/*.map` to display:
- OS region usage (`FLASH` 1 MB, `RAM` 96 KB, `CCMRAM` 64 KB, `APP_RAM` 32 KB, `EXTSRAM` 1 MB) and the largest symbols in each region.
- Every SDK app's `.bin` size, `.bss` size, total `APP_RAM` usage (out of 28 KB), and `.ext_bss` usage (out of 512 KB).

```powershell
python tools/map_budget.py --build-dir build/Debug --top 10
```

---

## 5. VS Code Tasks & One-Click Debugging (`.vscode/`)

- **`Ctrl+Shift+B`** (**Build Firmware & SDK Apps (Debug)**): Compiles the OS firmware and all SDK `.bin` apps.
- **`Terminal -> Run Task...`**:
  - **Upload & Run SDK App over USART3**: Prompts for an SDK app name from a dropdown, builds it, and uploads + runs it via `uart_upload.py`.
  - **Memory Budget Report (`map_budget.py`)**: Prints the full memory budget table.
- **`Run and Debug` (`F5`)**:
  - **Cortex Debug + SDK App Symbols (`0x20018000`)**: Flashes the OS, prompts you to pick an SDK app, loads its debug symbols at `0x20018000` (`add-symbol-file`), and sets a breakpoint at `app_main`.
  - **Cortex Attach + SDK App Symbols (`0x20018000`)**: Attaches to a running console without resetting it and loads the selected SDK app's symbols at `0x20018000`.
