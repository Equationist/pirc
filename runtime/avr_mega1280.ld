/*
 * Linker script for bare-metal ATmega1280 (QEMU -M mega):
 *   FLASH : VMA 0x000000 .. 0x01FFFF (128 KiB, 2-byte PC)
 *   SRAM  : VMA 0x800200 .. 0x8021FF (Harvard 0x800000 offset; 16-bit addr 0x0200..0x21FF)
 */

ENTRY(_vectors)

PROVIDE(__do_copy_data = 0);
PROVIDE(__do_clear_bss = 0);

MEMORY {
    FLASH (rx) : ORIGIN = 0x000000, LENGTH = 128K
    SRAM  (rw) : ORIGIN = 0x800200, LENGTH = 8K
}

SECTIONS {
    .text 0x000000 : {
        KEEP(*(.vectors))
        *(.text.startup)
        *(.text .text.*)
        *(.progmem .progmem.*)
        . = ALIGN(2);
        _etext = .;
    } > FLASH

    .data : {
        __data_start = .;
        *(.rodata .rodata.*)
        *(.data.rel.ro .data.rel.ro.*)
        *(.data .data.*)
        *(.tdata .tdata.*)
        __data_end = .;
    } > SRAM AT> FLASH

    __data_load_start = LOADADDR(.data);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(.tbss .tbss.*)
        *(COMMON)
        __bss_end = .;
    } > SRAM

    __heap_start = __bss_end;
    __heap_end   = 0x801A00;
    __stack_top  = 0x8021FF;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
