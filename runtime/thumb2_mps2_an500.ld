/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Linker script for QEMU ARM MPS2-AN500 (Cortex-M7, ARMv7E-M Thumb-2).
 *   FLASH : 0x00000000 .. 0x003FFFFF (4 MiB)
 *   RAM   : 0x20000000 .. 0x203FFFFF (4 MiB)
 */

ENTRY(_start)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

SECTIONS
{
    .text : ALIGN(4)
    {
        KEEP(*(.vectors))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    .data : ALIGN(4)
    {
        __data_start = .;
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH

    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(8)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM

    __heap_start = ALIGN(8);
    __heap_end   = ORIGIN(RAM) + LENGTH(RAM) - 0x80000; /* Reserve 512 KiB for stack */
    __stack_top  = ORIGIN(RAM) + LENGTH(RAM) - 8;       /* 8-byte aligned SP */
}
