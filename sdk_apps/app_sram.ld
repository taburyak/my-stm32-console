ENTRY(_start)

MEMORY
{
    /* 16 KB 0-Wait-State Code/Data Sandbox in STM32F407 Internal SRAM2 */
    SANDBOX_RAM (rwx) : ORIGIN = 0x20018000, LENGTH = 16K
}

SECTIONS
{
    .text 0x20018000 :
    {
        KEEP(*(.text.entry))
        *(.text*)
        *(.rodata*)
    } > SANDBOX_RAM

    .data :
    {
        *(.data*)
    } > SANDBOX_RAM

    .bss :
    {
        *(.bss*)
        *(COMMON)
    } > SANDBOX_RAM
}
