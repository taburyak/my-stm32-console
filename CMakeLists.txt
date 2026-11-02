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
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc
    Libs/ILI9341/inc
    Libs/XPT2046
    Libs/AT24XXX
    Libs/W25QXX
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
