# Cyber Arkanoid (SDK Application for STM32 Console)

## Overview
`arkanoid.c` is an unprivileged SDK game for **my-stm32-console** (STM32F407ZGT6). The loader copies it into the
internal SRAM sandbox (`0x20018000`) and runs it through the system calls in `console_sdk.h`.

The same source is also built into the firmware by CMake and shown in the Games launcher as the built-in game,
so it is available without an SD card.

## Gameplay
- 4 x 8 brick wall (top rows give more points); a cleared wall respawns and the ball speeds up.
- Paddle: Left / Right (gamepad, keyboard arrows / A-D, board keys); launch the ball with A / Space.
- High score is saved to `/saves/arkanoid.sav` (needs an SD card).
- Exit: ESC / gamepad B / board KEY3.

## Build for the SD card

```bash
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -O2 -g3 -nostdlib \
  "-Wl,--no-warn-rwx-segments" -T app_sram.ld arkanoid.c -o arkanoid.elf
arm-none-eabi-objcopy -O binary arkanoid.elf arkanoid.bin
```

Copy `arkanoid.bin` (and optionally a 64x64 BMP icon `arkanoid.bmp`) to `/games` on the SD card.
