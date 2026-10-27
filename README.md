# STM32F407 Console OS

Hobby game & tool console on an STM32F407ZGT6 development board: FreeRTOS, LVGL 8.3 UI on an ILI9341 320×240 FSMC display,
1 MB external SRAM, USB host HID (keyboard, mouse, gamepad), SD card (FatFs), and native `.bin` apps running in an unprivileged MPU sandbox.

## Documentation

- [docs/PROJECT_REFERENCE.md](docs/PROJECT_REFERENCE.md) — hardware, build, source map, tasks, memory map, syscalls, pitfalls.
- [docs/ROADMAP.md](docs/ROADMAP.md) — planned work.
- [docs/architecture/os-platform-guide.md](docs/architecture/os-platform-guide.md) — kernel, MPU, display and task architecture as built.
- [docs/architecture/memory-and-resource-strategy.md](docs/architecture/memory-and-resource-strategy.md) — memory budget and decisions.
- [my-stm32-console-rule.md](my-stm32-console-rule.md) — development rules.

## Building

The project uses CMake + Ninja and the `arm-none-eabi` toolchain:

```bash
cmake --preset Debug
cmake --build build/Debug
```

SDK apps (`sdk_apps/`) are built separately; see [docs/PROJECT_REFERENCE.md](docs/PROJECT_REFERENCE.md) §10.

## License

See the individual driver and library directories for their licenses (ILI9341, AT24XXX, LVGL, TinyUSB, FreeRTOS, STM32 HAL).
