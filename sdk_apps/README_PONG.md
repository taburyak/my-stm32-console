# Cyber-Pong Extended (SDK Application for STM32 Console)

## Overview
`pong_ext.c` is an unprivileged standalone game for **my-stm32-console** (STM32F407ZET6). It runs directly from External SRAM (`0x68080000`) or SD Card (`/games/pong.bin`) and utilizes System Calls (`console_sdk.h`) for graphics, hardware timing, audio, and input.

## New Features Added
1. **Real-time Score Counter**:
   - Increments every time the ball hits the paddle.
   - Instant on-screen display update using a custom embedded **5x7 bitmap font engine**.
2. **High Score Tracking**:
   - Tracks the highest score (`BEST`) achieved during the session.
3. **Progressive Difficulty**:
   - Ball speed slightly increases every 5 successful hits to keep gameplay challenging.
4. **Enhanced Visual & Sound Effects**:
   - Cyberpunk palette (Neon Green, Magenta, Mint, Gold).
   - "GAME OVER" banner on miss with automatic round restart.
   - Acoustic feedback via piezo beeper (`sys_beep`) and LED status toggling (`sys_led_toggle`).

## How to Compile `pong_ext.c` into `pong.bin`

Run the ARM GCC toolchain command in VS Code Terminal / Command Prompt:

```bash
# 1. Compile C source to ELF with debug symbols (-g3) for GDB
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -O2 -g3 -nostdlib -T app_sram.ld pong_ext.c -o pong.elf

# 2. Extract raw binary file for SD card deployment
arm-none-eabi-objcopy -O binary pong.elf pong.bin
```

## Deployment
1. Copy `pong.bin` to the `/games/` folder on your SD card.
2. Insert SD card into the console, select **Games** -> **pong.bin**, and press **A / Enter** to launch!
