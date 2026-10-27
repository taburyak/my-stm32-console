# console_sdk_app(<name> <source>): builds ${CMAKE_BINARY_DIR}/sdk_apps/<name>.bin (header + CRC) for the app sandbox

set(SDK_APPS_SRC_DIR "${CMAKE_SOURCE_DIR}/sdk_apps")
set(SDK_APPS_OUT_DIR "${CMAKE_BINARY_DIR}/sdk_apps")
file(MAKE_DIRECTORY "${SDK_APPS_OUT_DIR}")

set(SDK_APP_CFLAGS
    -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
    -O2 -g -std=gnu11 -ffreestanding -nostdlib -Wall)

set(SDK_APP_COMMON_DEPS
    "${SDK_APPS_SRC_DIR}/console_sdk.h"
    "${SDK_APPS_SRC_DIR}/console_abi.h"
    "${SDK_APPS_SRC_DIR}/sdk_runtime.c"
    "${SDK_APPS_SRC_DIR}/app_sram.ld"
    "${SDK_APPS_SRC_DIR}/sdk_app_crc.cmake")

function(console_sdk_app name src)
    set(elf "${SDK_APPS_OUT_DIR}/${name}.elf")
    set(bin "${SDK_APPS_OUT_DIR}/${name}.bin")
    add_custom_command(
        OUTPUT  "${bin}"
        COMMAND ${CMAKE_C_COMPILER} ${SDK_APP_CFLAGS} "-DSDK_APP_NAME_STR=\"${name}\""
                -I "${SDK_APPS_SRC_DIR}"
                -Wl,--no-warn-rwx-segments "-Wl,-Map=${SDK_APPS_OUT_DIR}/${name}.map"
                -T "${SDK_APPS_SRC_DIR}/app_sram.ld"
                "${src}" "${SDK_APPS_SRC_DIR}/sdk_runtime.c" -o "${elf}"
        COMMAND ${CMAKE_COMMAND} "-DELF=${elf}" "-DBIN=${bin}"
                "-DCC=${CMAKE_C_COMPILER}" "-DOBJCOPY=${CMAKE_OBJCOPY}"
                -P "${SDK_APPS_SRC_DIR}/sdk_app_crc.cmake"
        DEPENDS "${src}" ${SDK_APP_COMMON_DEPS}
        COMMENT "Building SDK app ${name}.bin"
        VERBATIM)
    add_custom_target(sdk_app_${name} ALL DEPENDS "${bin}")
endfunction()
