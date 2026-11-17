cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME my-stm32-console)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Project-owned linker script: replaces the CubeMX one (-T ...STM32F407xx_FLASH.ld) set in the toolchain file
set(CONSOLE_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/linker/STM32F407ZG_console.ld")
string(REGEX REPLACE "-T *\"[^\"]*\\.ld\"" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
string(REGEX REPLACE "-T *[^ \"]*\\.ld" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-T${CONSOLE_LINKER_SCRIPT}")
set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS "${CONSOLE_LINKER_SCRIPT}")

# SDK apps: built into ${CMAKE_BINARY_DIR}/sdk_apps/<name>.bin (copy to SD card games/)
include(sdk_apps/console_sdk_app.cmake)
console_sdk_app(arkanoid  "${CMAKE_SOURCE_DIR}/sdk_apps/arkanoid.c")
console_sdk_app(tetris    "${CMAKE_SOURCE_DIR}/sdk_apps/tetris.c")
console_sdk_app(sdk_test  "${CMAKE_SOURCE_DIR}/sdk_apps/sdk_test.c")
console_sdk_app(crash_test "${CMAKE_SOURCE_DIR}/sdk_apps/crash_test.c")
console_sdk_app(file_test "${CMAKE_SOURCE_DIR}/sdk_apps/file_test.c")
console_sdk_app(storage_test "${CMAKE_SOURCE_DIR}/sdk_apps/storage_test.c")
console_sdk_app(media_test "${CMAKE_SOURCE_DIR}/sdk_apps/media_test.c")
console_sdk_app(snake     "${CMAKE_SOURCE_DIR}/sdk_apps/snake.c")
console_sdk_app(invaders  "${CMAKE_SOURCE_DIR}/sdk_apps/invaders.c")
console_sdk_app(flappy    "${CMAKE_SOURCE_DIR}/sdk_apps/flappy.c")
console_sdk_app(pacman    "${CMAKE_SOURCE_DIR}/sdk_apps/pacman.c")
console_sdk_app(loderunner "${CMAKE_SOURCE_DIR}/sdk_apps/loderunner.c")
console_sdk_app(arkanoid_deluxe "${CMAKE_SOURCE_DIR}/sdk_apps/arkanoid_deluxe.c")
console_sdk_app(stack_up "${CMAKE_SOURCE_DIR}/sdk_apps/stack_up.c")
console_sdk_app(sram_test "${CMAKE_SOURCE_DIR}/sdk_apps/sram_test.c")


# Built-in (ROM) SDK app: arkanoid.bin is embedded into Flash
set(ROM_APPS_DIR      "${CMAKE_BINARY_DIR}/rom_apps")
set(ROM_ARKANOID_BIN  "${SDK_APPS_OUT_DIR}/arkanoid.bin")
set(ROM_APPS_ASM      "${ROM_APPS_DIR}/rom_apps.S")
file(MAKE_DIRECTORY "${ROM_APPS_DIR}")
file(WRITE "${ROM_APPS_ASM}"
    "    .section .rodata.rom_apps,\"a\"\n"
    "    .balign 4\n"
    "    .global g_rom_arkanoid_bin\n"
    "g_rom_arkanoid_bin:\n"
    "    .incbin \"${ROM_ARKANOID_BIN}\"\n"
    "    .global g_rom_arkanoid_bin_end\n"
    "g_rom_arkanoid_bin_end:\n")
set_source_files_properties("${ROM_APPS_ASM}" PROPERTIES OBJECT_DEPENDS "${ROM_ARKANOID_BIN}")
target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${ROM_APPS_ASM}")
add_dependencies(${CMAKE_PROJECT_NAME} sdk_app_arkanoid)

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Automatically locate all source files for the LVGL library
file(GLOB_RECURSE LVGL_SOURCES "Libs/lvgl/src/*.c")

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${LVGL_SOURCES}
    Core/Src/main.c
    Core/Src/stm32f4xx_it.c
    Core/Src/stm32f4xx_hal_msp.c
    Libs/ILI9341/src/ili9341_simple.c
    Libs/XPT2046/XPT2046_touch.c
    Libs/AT24XXX/AT24Cxx.c
    Libs/W25QXX/w25qxx.c
    Libs/DS18B20/onewire.c
    Libs/DS18B20/ds18b20.c
    FATFS/App/fatfs.c
    FATFS/Target/sd_diskio.c
    FATFS/Target/bsp_driver_sd.c
    Libs/lvgl_port/lv_port_console.c
    Core/Src/status_led.c
    Core/Src/buttons_manager.c
    Core/Src/beeper.c
    Core/Src/system_post.c
    Core/Src/ccmram.c
    Core/Src/sram_ext.c
    Core/Src/sdcard_fatfs.c
    Core/Src/usb_host_manager.c
    Core/Src/os_tasks.c
    Libs/tinyusb/src/tusb.c
    Libs/tinyusb/src/common/tusb_fifo.c
    Libs/tinyusb/src/host/usbh.c
    Libs/tinyusb/src/host/hub.c
    Libs/tinyusb/src/class/hid/hid_host.c
    Libs/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c
    Libs/tinyusb/src/portable/synopsys/dwc2/hcd_dwc2.c
    Libs/tinyusb/src/portable/synopsys/dwc2/dwc2_common.c
    Core/Src/ui_manager.c
    Core/Src/app_loader.c
    Core/Src/app_files.c
    Core/Src/app_storage.c
    Core/Src/app_services.c
    Core/Src/script_engine.c
    Core/Src/uart_loader.c
    Core/Src/uart_cli.c
    Core/Src/temp_sensor.c
    Core/Src/esp_companion.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc
    Libs/ILI9341/inc
    Libs/XPT2046
    Libs/AT24XXX
    Libs/W25QXX
    Libs/DS18B20
    FATFS/App
    FATFS/Target
    Libs/lvgl
    Libs/lvgl_port
    Libs/tinyusb/src
    Libs/tinyusb/src/common
    Libs/tinyusb/src/host
    Libs/tinyusb/src/class/hid
    Libs/tinyusb/src/portable/synopsys/dwc2
    sdk_apps
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# --------------------------------------------------------------------------
# Bootloader (0x08000000, 64 KB) + OTA Package (fw_update.bin) + Full Image
# --------------------------------------------------------------------------
set(BOOT_TARGET console_bootloader)
add_executable(${BOOT_TARGET}
    bootloader/Src/boot_main.c
    bootloader/Src/boot_sd_diskio.c
    Libs/ILI9341/src/ili9341_simple.c
    Core/Src/system_stm32f4xx.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    startup_stm32f407xx.s
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_sd.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_sdmmc.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_sram.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_fsmc.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c
    Middlewares/Third_Party/FatFs/src/diskio.c
    Middlewares/Third_Party/FatFs/src/ff.c
    Middlewares/Third_Party/FatFs/src/ff_gen_drv.c
    Middlewares/Third_Party/FatFs/src/option/ccsbcs.c
)
target_include_directories(${BOOT_TARGET} PRIVATE
    bootloader/Inc
    Core/Inc
    Libs/ILI9341/inc
    Middlewares/Third_Party/FatFs/src
    Drivers/STM32F4xx_HAL_Driver/Inc
    Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
    Drivers/CMSIS/Device/ST/STM32F4xx/Include
    Drivers/CMSIS/Include
)
target_compile_definitions(${BOOT_TARGET} PRIVATE
    USE_HAL_DRIVER
    STM32F407xx
    BOOTLOADER_BUILD
)
target_compile_options(${BOOT_TARGET} PRIVATE -Os)

set(BOOT_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/bootloader/STM32F407ZG_boot.ld")
set_target_properties(${BOOT_TARGET} PROPERTIES LINK_FLAGS "-T${BOOT_LINKER_SCRIPT}")
set_property(TARGET ${BOOT_TARGET} APPEND PROPERTY LINK_DEPENDS "${BOOT_LINKER_SCRIPT}")
target_link_libraries(${BOOT_TARGET} ${TOOLCHAIN_LINK_LIBRARIES})

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(TARGET ${BOOT_TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${BOOT_TARGET}> "${CMAKE_BINARY_DIR}/console_bootloader.bin"
    COMMENT "Generating console_bootloader.bin"
)
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/fw_update.bin" "${CMAKE_BINARY_DIR}/console_full.bin"
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> "${CMAKE_BINARY_DIR}/my-stm32-console.bin"
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${BOOT_TARGET}> "${CMAKE_BINARY_DIR}/console_bootloader.bin"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/pack_fw.py"
            "${CMAKE_BINARY_DIR}/my-stm32-console.bin" "${CMAKE_BINARY_DIR}/fw_update.bin"
    COMMAND ${Python3_EXECUTABLE} -c "import pathlib; b=pathlib.Path('${CMAKE_BINARY_DIR}/console_bootloader.bin').read_bytes().ljust(65536, b'\\xff'); a=pathlib.Path('${CMAKE_BINARY_DIR}/my-stm32-console.bin').read_bytes(); pathlib.Path('${CMAKE_BINARY_DIR}/console_full.bin').write_bytes(b+a)"
    DEPENDS ${CMAKE_PROJECT_NAME} ${BOOT_TARGET}
    COMMENT "Generating fw_update.bin and console_full.bin"
)
add_custom_target(console_images ALL DEPENDS "${CMAKE_BINARY_DIR}/fw_update.bin" "${CMAKE_BINARY_DIR}/console_full.bin")
