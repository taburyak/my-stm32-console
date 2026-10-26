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
