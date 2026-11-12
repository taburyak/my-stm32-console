# STM32F407 Console OS

DIY retro game & developer console built on **STM32F407ZGT6** (168 MHz Cortex-M4F) with an optional **ESP32-C6-LCD-1.47** Wi-Fi 6 / BLE 5 companion co-processor.

## Key Features

- **RTOS & MPU Sandbox**: FreeRTOS multitasking (`GuiRenderTask`, `InputAudioTask`, `StorageIoTask` in 0-WS CCM RAM) + hardware **MPU** isolation for unprivileged native `.bin` SDK apps (`APP_RAM` 32 KB + `EXT_APP` 512 KB).
- **Display & Touch UI**: LVGL 8.3 desktop on a 2.8" **ILI9341** 320×240 FSMC display with DMA2 double-buffered rendering, **XPT2046** 4-point touch calibration, 4 color themes, custom `.bmp` wallpapers, and a 2-page swipeable Home screen (`Launcher` + `Widgets`).
- **On-Device & Web Development**:
  - Built-in **C-Script interpreter** (`Core/Src/script_engine.c`) for running `.c` source games/tools directly from SD without a PC compiler.
  - On-console **SD File Manager & Code Editor** (new file/folder, rename, copy, move, delete, edit, save & run).
  - Wireless **Web Studio (`http://stm32-console.local`)** via ESP32-C6 over `USART6` (`PC6`/`PC7` @ `921600` baud): live telemetry, SD explorer, universal text/code editor, inline `.bmp`/`.png`/`.jpg` image viewer, live screen capture (`📸 Capture Screen`), virtual web gamepad (`🎮 Gamepad`), wallpaper setter, and instant `Save & Run`.
- **Input & Peripherals**:
  - **USB Host (TinyUSB)**: USB HID Keyboard, Mouse (with live cursor), and Gamepad (`D-Pad` + dual analog sticks).
  - **Storage**: microSD card (`SDIO` 4-bit DMA + FatFs), `W25Q128` SPI Flash (16 MB), `AT24C02` I2C EEPROM (settings persistence).
  - **Sensors, Time & SpeedTest**: Hardware RTC with automatic **Wi-Fi SNTP** sync, **Open-Meteo** 3-day outdoor weather station, **Network & USART6 SpeedTest** widget, onboard **DS18B20** room sensor (`PA15`), and internal MCU temperature monitor.
  - **Dual-Screen Companion HUD (`ESP32-C6-LCD-1.47`)**: 3-page 172×320 ST7789 status display switched via `BOOT` button + programmable **Game HUD Mode** (`esp_hud_text`, `esp_led`) for native SDK and C-Script apps.

## Controls

- **Physical Board Keys (`PF6..PF9`)**:
  - `KEY2` long press (or `F12` / `PrtSc` / Gamepad `Select + R1`): Capture screen to `SD:/screenshots/*.bmp`.
  - `KEY3` long press (or `ESC` / Gamepad `Start + Select`): Force-exit running app/game back to OS.
- **Desktop Navigation**: Touch, USB Mouse, USB Keyboard (`Arrows`, `Enter`, `Esc`), or USB Gamepad (`D-Pad` / `Left Stick`, `A` = Open, `B` = Back).

## SD Card Layout

- `/games` — Native `.bin` SDK games, `.c` C-Script games, and optional `<game>.bmp` 64×64 icons.
- `/assets` — Wallpapers (`wall.bmp`) and optional custom weather icons (`/assets/weather/*.bmp`).
- `/saves` & `/appdata` — Per-app persistent high scores and save states.
- `/screenshots` — Saved 24-bit `.bmp` screen captures.
- `/system` — `wifi.cfg` and `wallpaper.cfg`.

## Documentation & Building

- [doc/C_SCRIPT_MANUAL.md](doc/C_SCRIPT_MANUAL.md) — C-Script language & API manual.
- [sdk_apps/README_ARKANOID.md](sdk_apps/README_ARKANOID.md), [sdk_apps/README_TETRIS.md](sdk_apps/README_TETRIS.md) — SDK game guides.

```bash
cmake --preset Debug
cmake --build build/Debug
```

## License

See individual driver and library directories for their respective licenses (ILI9341, AT24XXX, LVGL, TinyUSB, FreeRTOS, STM32 HAL).
