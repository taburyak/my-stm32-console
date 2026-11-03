# STM32F407 Console OS

Hobby game & tool console on an STM32F407ZGT6 development board: FreeRTOS, LVGL 8.3 UI on an ILI9341 320×240 FSMC display,
1 MB external SRAM, USB host HID (keyboard, mouse, gamepad), SD card (FatFs), and native `.bin` apps running in an unprivileged MPU sandbox.

## Documentation

- [doc/C_SCRIPT_MANUAL.md](doc/C_SCRIPT_MANUAL.md) — user manual and API reference for writing C-Script (`*.c`) games and apps.
- [sdk_apps/README_ARKANOID.md](sdk_apps/README_ARKANOID.md), [sdk_apps/README_TETRIS.md](sdk_apps/README_TETRIS.md) — SDK game guides.
- Local working notes (if present): `docs/PROJECT_REFERENCE.md`, `docs/ROADMAP.md`.

## Building

The project uses CMake + Ninja and the `arm-none-eabi` toolchain:

```bash
cmake --preset Debug
cmake --build build/Debug
```

SDK apps (`sdk_apps/`) are built separately; see [docs/PROJECT_REFERENCE.md](docs/PROJECT_REFERENCE.md) §10.

## License

See the individual driver and library directories for their licenses (ILI9341, AT24XXX, LVGL, TinyUSB, FreeRTOS, STM32 HAL).
