ENTRY(_start)

MEMORY
{
    /* 28 KB 0-WS internal SRAM for code, constants, data & BSS (top 4 KB of the 32 KB sandbox is the task stack) */
    APP_RAM (xrw) : ORIGIN = 0x20018000, LENGTH = 28K
    /* 512 KB External FSMC SRAM for Large Game Buffers & Assets */
    EXT_RAM (rw)  : ORIGIN = 0x68080000, LENGTH = 512K
}

SECTIONS
{
    .text : ALIGN(4)
    {
        KEEP(*(.text.entry))
        *(.text*)
        . = ALIGN(4);
        *(.rodata*)
        . = ALIGN(4);
    } > APP_RAM

    .data : ALIGN(4)
    {
        *(.data*)
        . = ALIGN(4);
    } > APP_RAM

    /* Disallow C++ exception tables / unwinding that bloat raw binaries */
    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.eh_frame*)
    }

    .bss (NOLOAD) : ALIGN(4)
    {
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
    } > APP_RAM

    .ext_bss (NOLOAD) : ALIGN(4)
    {
        *(.ext_bss*)
        . = ALIGN(4);
    } > EXT_RAM
}
