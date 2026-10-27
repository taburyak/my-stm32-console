# Cyber Arkanoid (SDK Application for STM32 Console)

## Overview
`arkanoid.c` is an unprivileged SDK game for **my-stm32-console** (STM32F407ZGT6). The loader copies it into the
internal SRAM sandbox (`0x20018000`) and runs it through the system calls in `console_sdk.h`.

The same source is also built into the firmware by CMake and shown in the Games launcher as the built-in game,
so it is available without an SD card.

## Gameplay
- 4 level patterns of 6 x 8 bricks; silver-framed bricks need two hits. Each level is faster.
- 3 lives. The bounce angle depends on where the ball hits the paddle; side hits on bricks reflect horizontally.
- Paddle: Left / Right (gamepad, keyboard arrows / A-D, board keys), USB mouse or touch screen.
- Launch: A / Space / board UP / mouse click / tap. Pause: gamepad L2/R2 or key P.
- Best score: `SD:/appdata/arkanoid/best.dat` (imported once from the old `/saves/arkanoid.sav`).
- Exit: ESC / gamepad B / board KEY3.

## Build for the SD card

Built together with the firmware (CMake target `sdk_app_arkanoid`) into `build/<preset>/sdk_apps/arkanoid.bin`.
The image carries the SDK app header and CRC; files built without the SDK runtime are rejected by the loader.

Copy `arkanoid.bin` (and optionally a 64x64 BMP icon `arkanoid.bmp`) to `/games` on the SD card.
