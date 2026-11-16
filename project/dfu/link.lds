#include "ptab.h"

ENTRY(dfu_reset)

MEMORY
{
    IMAGE (rwx) : ORIGIN = DFU_EXEC_START_ADDR, LENGTH = DFU_LOADER_SIZE - 0x2000
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 0x4000
    STACK (rw)  : ORIGIN = 0x2001C000, LENGTH = 0x4000
    WORK  (rw)  : ORIGIN = DFU_EXEC_START_ADDR + DFU_LOADER_SIZE, LENGTH = 0x1C0000
}

SECTIONS
{
    .vectors : { KEEP(*(.dfu_vectors)) } > IMAGE
    .text :
    {
        *(.text .text.* .rodata .rodata.*)
        *(.ARM.extab*)
    } > IMAGE
    .ARM.exidx : { *(.ARM.exidx*) } > IMAGE
    .data :
    {
        *(.data .data.* .sdata .sdata.*)
        *(.retm_data .retm_data.* .l1_ret_data*)
    } > IMAGE
    .flash_ops :
    {
        __flash_ops_start = .;
        *(.dfu_flash_ops)
        . = ALIGN(4);
        __flash_ops_end = .;
    } > SRAM AT > IMAGE
    __flash_ops_load = LOADADDR(.flash_ops);
    .bss (NOLOAD) :
    {
        . = ALIGN(32);
        __bss_start__ = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        *(.retm_bss .retm_bss.* .l1_ret_bss*)
        . = ALIGN(32);
        __bss_end__ = .;
    } > WORK
    __stack_limit = ORIGIN(STACK);
    __stack_top = ORIGIN(STACK) + LENGTH(STACK);
}
