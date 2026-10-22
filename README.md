# STM32F407 Console OS Project

This project is a functional "Console OS" prototype for the STM32F407 Discovery (or compatible) board, featuring a GUI, USB Host support, external memory management, and MPU-based security.

## 🚀 Features

- **FreeRTOS Integration**: Multitasking environment with several specialized tasks.
- **Graphic Engine**: 320x240 LCD support (ILI9341) with DMA-accelerated frame buffer rendering from external SRAM.
- **USB Host**: Support for USB Keyboards (HID class).
- **Security**: Hardware isolation using the **MPU (Memory Protection Unit)**. A "Sandbox" task runs in unprivileged mode to demonstrate system protection.
- **Memory Management**: 
    - **Internal SRAM**: Used for kernel and critical tasks.
    - **External SRAM (1MB)**: Used for frame buffers and user heap (Heap_5).
    - **EEPROM (AT24Cxx)**: Support for non-volatile storage via I2C.
- **Diagnostics**: UART console (115200 baud) for real-time debugging and system logs.

## 🛠 Hardware Used

- **MCU**: STM32F407VGT6
- **Display**: ILI9341 (FSMC Interface)
- **Touch**: XPT2046 (SPI)
- **EEPROM**: AT24Cxxx (I2C1)
- **External RAM**: 1MB IS62WV51216 (FSMC Bank 3)
- **Debug Port**: USART3 (for `printf` output)

## 📂 Project Structure

- `Core/Src/main.c`: Main entry point and task management.
- `Libs/ILI9341`: LCD driver.
- `Libs/XPT2046`: Touch screen driver.
- `Libs/AT24XXX`: EEPROM driver.
- `USB_HOST/`: STM32 USB Host stack configuration.

## 🎮 Controls (Buttons)

- **KEY0 (PF6)**: Toggle LCD Backlight.
- **KEY1 (PF7)**: Cycle through LCD color schemes (Gradient palettes).
- **KEY2 (PF8)**: Shift color palette (Animation test).
- **KEY3 (PF9)**: Run **EEPROM Diagnostic** (Write/Read test) and check External SRAM.

## 🛠 Building

The project uses **CMake** and the **arm-none-eabi** toolchain.

1. Ensure you have the ARM GCC toolchain installed.
2. Build using CMake:
   ```bash
   cmake -B build -G Ninja
   cmake --build build
   ```

## 📝 License

This project is open-source. See the repository for specific driver licenses (ILI9341, AT24XXX).
