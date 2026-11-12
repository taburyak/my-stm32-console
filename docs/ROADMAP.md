# Roadmap

Single living list of planned work. Analysis behind items: [MASTER_PLAN.md](MASTER_PLAN.md), [architecture/memory-and-resource-strategy.md](architecture/memory-and-resource-strategy.md).
Legend: `[x]` done · `[~]` in progress · `[ ]` planned.

## Done — safety, SDK v2/v3 & games

- [x] Memory step M0: `memory_map.h`, linker reservation of the app sandbox, heap overlap fix, buffer validation, sandbox stack inside the sandbox (step 1); project-owned linker script, SVCall priority 14 after scheduler start, `svc 0` only from MSP (step 2); syscall gate (step 3); task-per-launch lifecycle, fault containment, KEY3 kill (step 4); MPU narrowing + crash test cases (step 5).
- [x] Memory step M1 (CCM rebalance, step 6): FreeRTOS internal heap (`ucHeapInternal`, 15 KB) and `g_dir_cache` (4.4 KB) moved to CCM RAM.
- [x] Syscall gate for blocking calls: `save/load_state`, `sys_sleep_ms`, `sys_lcd_blit`, file API (SVC 31–38), and EEPROM / W25Q flash APIs (SVC 40–44).
- [x] Games launcher with icon tiles, built-in Arkanoid (ROM app built from `sdk_apps/arkanoid.c`), "Set as wallpaper" in the BMP viewer.
- [x] SDK v2 part 1: `console_abi.h`, gamepad / mouse / info syscalls, `sys_sleep_ms`.
- [x] SDK v2 part 2: app header (`SDK_AppHeader_t`) + CRC-32 (`sdk_apps/sdk_app_crc.cmake`), `app_main()` runtime (`sdk_runtime.c`), CMake build of all apps (`sdk_apps/console_sdk_app.cmake`).
- [x] SDK v2 part 3: file API (SVC 31–38) with per-app SD namespace (`SD:/appdata/<name>/` and `res:` → `SD:/games/<name>/`).
- [x] SDK v2 part 4: EEPROM window (`0x80..0xFF`) and W25Q partition (SVC 40–44).
- [x] SDK v2 part 5 (API v3): hardware RNG (`sys_random`), input snapshot (`sys_input_get_state`), LCD blit (`sys_lcd_blit`), note sequences (`sys_audio_play`), GPIO skeleton.
- [x] SDK game helpers (`sdk_frame_sync` frame pacing, `SDK_Input_t` with DAS/ARR auto-repeat) + games: Snake (`snake.c`), Tetris rewrite (`tetris.c`), Arkanoid rewrite (`arkanoid.c`), Space Invaders (`invaders.c`), Flappy Bird (`flappy.c`), Pac-Man (`pacman.c`), and Lode Runner (`loderunner.c`).
- [x] IWDG for the kernel (`MX_IWDG_Init`, refreshed in `StartDefaultTask` and during app load/fault handling).
- [x] FreeRTOS runtime statistics via 32-bit `TIM5` (`configGENERATE_RUN_TIME_STATS = 1`) + live CPU load & free heap display in SysInfo.

- [x] Non-blocking LCD flush (`DMA2_Stream0` IRQ → `lv_disp_flush_ready` with a single 320×40 buffer in `MM_EXT_LVGL_DRAW_BUF`, configurable via `LVGL_PORT_USE_DMA_IT`).
- [x] DMA fill for SVC 5/12 (`lcdFillConstantDMA`: mem-to-mem with fixed source for ≥ 256 pixels).
- [x] Gamepad & Keyboard SDK expansion: analog sticks (`left_x/y`, `right_x/y`), `SDK_PAD_L3/R3`, and raw 8-byte HID report in `SDK_GamepadState_t`; raw keyboard press/release event queue via `sys_get_key_event` (SVC 22).
- [x] Built-in C-Script interpreter (`Core/Src/script_engine.c`): executes `/games/*.c` and SD Explorer `.c` scripts in unprivileged `SandboxTask` from MCU Flash (0 WS) with source/tokens/symbols/arrays in the 512 KB external SRAM sandbox (`0x68080000`); includes built-in `C-Demo.c` ROM script and `sdk_apps/star_catcher.c`.
- [x] Full external SRAM test (data bus D0..D15 + NBL0/1, address bus A0..A18, 512 KB PRBS-32 March) in `system_post.c` + interactive "Test SRAM" button and SDK API version in SysInfo (`ui_manager.c`) + under-load FSMC DMA/SDIO stress test app (`sdk_apps/sram_test.c`).
- [x] Asset conversion tooling (`tools/img2rgb565.py`: PNG/BMP/JPG → RGB565 C header, `.raw`, `.simg`, or 16-bit `.bmp` icons/wallpapers with optional Bayer dithering), linker map budget analyzer (`tools/map_budget.py`), and debug automation (`.vscode/launch.json` with `add-symbol-file` at `0x20018000` + `.vscode/tasks.json`).
- [x] Upload `.bin` / `.c` / `.bmp` over USART3 (`Core/Src/uart_loader.c` + `tools/uart_upload.py`): supports both zero-touch framed upload (`"UPLD!\n"` auto-wake + IEEE CRC-32 + save to SD + instant RAM launch) and standard XMODEM-CRC / XMODEM-1K via the **UART Rx** tile in Games Hub.
- [x] Screenshot to SD (`SD:/screenshots/shot_NNN.bmp`) and live PC capture (`python tools/uart_upload.py --screenshot shot.bmp`): reads 320×240 GRAM via `ILI9341_MEMORYREAD` (`lcdReadPixelsRGB565`), triggered by `KEY2` long-press, USB keyboard `PrintScreen`/`F12`, Gamepad `Select + R1`, or `--screenshot` over `USART3`.
- [x] Internal MCU temperature sensor (`ADC1` `IN16`/`IN17` with `VREFINT` calibration + overheat alarm) & external 1-Wire `DS18B20` (`PA15`, `Libs/DS18B20`, `Core/Src/temp_sensor.c`), exposed to SDK apps via `sys_get_mcu_temp_deci()` / `sys_get_ext_temp_deci()` (SVC 49) and C-scripts (`temp_mcu()` / `temp_ext()`).
- [x] Settings & EEPROM persistence (`AT24C02` `0x00..0x0B`): 4-point `XPT2046` touchscreen calibration wizard (`Run_Touch_Calibration_Wizard`), gamepad analog stick deadzone configuration (`4..32`), and 4 UI color themes (`Cyber Blue`, `Emerald`, `Retro Amber`, `Synthwave`).
- [x] Interactive Text & C-Script Editor and File Manager in SD Explorer (`Core/Src/ui_manager.c`, `Libs/lvgl_port/lv_port_console.c`): multi-line `lv_textarea` editor (up to 8 KB in `MM_EXT_TEXT_PREVIEW`) with block cursor, USB keyboard navigation (`Up`/`Down`/`Left`/`Right`/`Home`/`End`/`Del`), toggleable on-screen touch `lv_keyboard`, `Save` to SD card, instant `Run` button for `.c` scripts, plus full file & directory operations (`[ + New File... ]` with custom name & supported extension selector `.c`/`.txt`/`.h`/`.md`/`.ini`/`.cfg`, `[ + New Folder... ]` via `f_mkdir`, `[ Rename Current Folder ]`, `[ Delete Empty Folder ]`, file `Rename`, `Copy`, `Move`, `Paste`, and `Delete` with confirmation).
- [x] Multi-page Home Dashboard (`lv_tileview` swipe left/right between 4-tile Launcher page and 4-tile Widgets page), full-screen Weather Station (`UI_SCREEN_WEATHER` with `Open-Meteo` outdoor weather + 3-day forecast via `ESP32-C6`, indoor `DS18B20` & `MCU` temperatures, procedural + SD `.bmp` weather icons), interactive Monthly Calendar (`UI_SCREEN_CALENDAR` synced with NTP/RTC), and 3-stage Network & `USART6` Companion Link SpeedTest (`UI_SCREEN_SPEEDTEST`).
- [x] `ESP32-C6-LCD-1.47` Wi-Fi 6 Companion Co-Processor on `USART6` (`PC6`/`PC7` @ `921600` baud, circular DMA RX + flow-controlled binary streaming): 3-page status HUD + Dual-Screen Game HUD & WS2812 RGB LED API (`SDK_SVC_ESP_CTL` / C-Script `esp_hud_*`, `esp_led`, `weather_temp`), plus **Web Studio (`http://stm32-console.local`)** with SD Explorer, universal editor, inline image viewer, live screen capture, and virtual web gamepad.

## Next

- [ ] Interactive PC Terminal CLI over `USART3` (`115200` / `921600`, alongside `"UPLD!\n"` binary loader):
  - Human-friendly text shell (`help` / `-help` / `/help`, `ls [dir]`, `cat <file>`, `rm <file>`, `run <app>`, `stop`, `sysinfo`, `temp`, `shot`, `reboot`) usable from any serial terminal (PuTTY, Tera Term, VS Code Serial Monitor) — especially useful when operating standalone without the ESP32-C6 Wi-Fi module.
  - Optional `esp <cmd>` bridge command to query/configure the companion module (Wi-Fi status, credentials, diagnostics) directly from the PC serial terminal.
- [ ] Firmware Update Architecture (Wi-Fi OTA & Serial Terminal DFU):
  - **ESP32-C6 Companion OTA**: HTTP firmware upload endpoint (`POST /api/ota/esp32` via Arduino `Update` partition API) in Web Studio + serial update via USB-C.
  - **STM32F407 Console Bootloader & OTA/UART DFU**: compact custom bootloader in Flash Sector 0–1 (`0x08000000`, 32 KB) with CRC-32 + header verification that flashes `SD:/system/fw_update.bin` into main application sectors (`0x08008000..0x080FFFFF`) with automatic rollback safety; firmware image can be staged either wirelessly via Web Studio (`POST /api/ota/stm32` → SD) or from PC terminal over `USART3` (XMODEM-1K / `tools/uart_upload.py`).
- [ ] Memory step M2-a (LVGL pool to external SRAM, 64 KB app sandbox) when needed.

## Later

- [ ] Audio: centralized `VS1053` MP3/WAV/MIDI/Ogg/FLAC audio decoder module on STM32 console (SPI + DREQ/XDCS/XCS pins, stereo 3.5mm jack / line-out):
  - System clicks and UI feedback.
  - In-game music, PCM SFX and General MIDI synth for SDK apps (`sys_audio_*`).
  - Internet Radio streaming: ESP32-C6 fetches HTTP audio stream over Wi-Fi, chunks it via framed UART4 packets into STM32 ring buffer, STM32 DMA streams to VS1053 via SPI.
  - Keeps wiring clean: audio decoder is strictly on the console board, only 4 wires remain between STM32 and ESP32.
  - Buzzer stays for hardware POST / fallback error alerts.
- [ ] LittleFS on W25Q128 + VFS (`/sd`, `/flash`); key-value store for settings and saves.
- [~] Wallpapers: single wallpaper from the BMP viewer and UI color themes done; per-screen wallpapers open.
- [ ] USB hub (multiple HID devices); USB MSC device mode to expose the SD card to a PC.
- [ ] TE (tearing effect) sync if the pin is routed on the board.
- [ ] Ethernet + LwIP: upload games over FTP/HTTP (if the PHY is present).
- [ ] Script VM for small apps (Lua / Berry / wasm3 / PicoC).
- [ ] App GPIO API (activate the skeleton) + DS18B20 example.
- [ ] Relocatable modules or overlays for apps larger than the sandbox.
- [ ] UART link multiplayer between two consoles.
- [~] Rich LVGL apps: text & C-script editor, Weather Station, Monthly Calendar, and SpeedTest done; Google Calendar event sync open.

## Engineering hygiene

- [x] Release (`-Os`) profile for daily use; Debug with `-Og` (`cmake/gcc-arm-none-eabi.cmake`).
- [x] Map-file analysis script with memory budget checks (`tools/map_budget.py`).
- [ ] `-Wextra`, cppcheck / clang-tidy, `.clang-format`.
- [x] Firmware version (`CONSOLE_FW_VERSION_STR`) & SDK API version (`SDK_API_VERSION`) shown in SysInfo.
- [ ] Host-side unit tests for game logic.
