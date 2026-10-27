# Cyber-Tetris for STM32 Console (SRAM App)

A fully featured, standalone 8-bit Cyber-Tetris application for the STM32F407 console.

## Features
- **7 Classic Tetromino Shapes** (I, J, L, O, S, T, Z) with 4-state rotations and wall-kick tolerance.
- **Dynamic Speed & Levels**: Fall speed increases as you clear lines (level up every 10 lines).
- **Next Piece Window**: Preview the upcoming piece.
- **Score & High Score**: Full score calculation based on single, double, triple, and 4-line Tetris clears.
- **Soft & Hard Drops**: Tap Down for soft drop or B for instant Hard Drop.
- **Sound & LED Feedback**: Beeper alerts on rotation, drop, line clear, and game over.

## Controls
- **D-Pad Left / Right**: Move piece
- **D-Pad Down**: Soft drop
- **Button A / D-Pad Up**: Rotate piece clockwise
- **Button B**: Hard drop (instant)
- **Button Home / Exit**: Return to Console Main Menu

## How to Build
Built together with the firmware (CMake target `sdk_app_tetris`) into `build/<preset>/sdk_apps/tetris.bin`.

Copy `tetris.bin` to `/games/` on your SD card!
