/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bare-metal linker script for QEMU bamboo machine (powerpc-unknown-elf).
 * RAM starts at 0x00100000 (16 MiB).
 */
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x00100000, LENGTH = 16M
}

SECTIONS {
    . = ORIGIN(RAM);

    .text : {
        *(.text.boot)
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > RAM

    .data : ALIGN(8) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } > RAM

    .tdata : ALIGN(8) {
        __tdata_start = .;
        *(.tdata .tdata.*)
        __tdata_end = .;
    } > RAM

    .tbss (NOLOAD) : ALIGN(8) {
        __tbss_start = .;
        *(.tbss .tbss.*)
        __tbss_end = .;
    } > RAM

    .bss (NOLOAD) : ALIGN(8) {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM

    . = ALIGN(16);
    __heap_start = .;
    __heap_end = ORIGIN(RAM) + LENGTH(RAM) - 0x40000; /* Reserve 256 KiB for stack */
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
