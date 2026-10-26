ENTRY(_start)

MEMORY
{
    SANDBOX_RAM (rwx) : ORIGIN = 0x68080000, LENGTH = 256K
}

SECTIONS
{
    .text 0x68080000 :
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