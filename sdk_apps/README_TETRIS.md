# Cyber-Tetris for STM32 Console (SRAM App)

A fully featured, standalone 8-bit Cyber-Tetris application for the STM32F407 console.

## Features
- 7-bag randomizer, SRS rotation states with simple wall kicks (0, ±1, ±2 columns, 1 row up).
- Lock delay 400 ms (reset by moves/rotations, up to 15 times).
- Levels every 10 lines; gravity 450 ms at level 0 down to 80 ms.
- Scoring: 40 / 100 / 300 / 1200 x (level + 1); soft drop +1 per cell, hard drop +2 per cell.
- Ghost piece, hold slot, next preview, line-clear flash, pause.
- Best score: `SD:/appdata/tetris/best.dat` (imported once from the old `/saves/tetris.sav`).

## Controls
- **Left / Right**: move (auto-repeat after 170 ms, every 50 ms)
- **Down**: soft drop
- **A / Up**: rotate clockwise (A or Up also starts a game)
- **B** (pad X, key X/Z): hard drop
- **X** (pad Y, key C): hold
- **START** (pad L2/R2, key P): pause
- **ESC** / pad B / board KEY3: exit

## How to Build
Built together with the firmware (CMake target `sdk_app_tetris`) into `build/<preset>/sdk_apps/tetris.bin`.

Copy `tetris.bin` to `/games/` on your SD card!
