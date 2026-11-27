/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Minimal bare-metal PowerPC 32-bit Big-Endian startup, Book-E trap handler,
 * dual-UART I/O & exit, and SysV setjmp/longjmp for QEMU bamboo
 * (-M bamboo -display none -no-reboot -bios none -serial stdio -serial file:<ec>).
 *
 * Peripherals on PPC440EP bamboo:
 *   UART0 (stdout):    0xef600300 (NS16550 THR)
 *   UART1 (exit code): 0xef600400 (1 byte written before DBCR0 reset)
 *   DBCR0 (SPR 308):   write 0x30000000 for immediate system reset (QEMU exits 0)
 */

    .section .text.boot,"ax",@progbits
    .globl _start
    .p2align 2
_start:
    /* Enable FPU (MSR.FP = 0x2000) so IEEE-754 hardware FP instructions do not trap. */
    mfmsr   3
    ori     3, 3, 0x2000
    mtmsr   3
    isync

    /* Install Book-E Program Exception handler (IVPR=SPR 63, IVOR6=SPR 406) for tw 31,0,0. */
    lis     3, _pir_ppc32_trap@h
    ori     3, 3, _pir_ppc32_trap@l
    rlwinm  4, 3, 0, 0, 15
    mtspr   63, 4
    andi.   4, 3, 0xFFFF
    mtspr   406, 4
    isync

    /* Initialize 16-byte aligned stack pointer with null backchain. */
    lis     1, _stack_top@h
    ori     1, 1, _stack_top@l
    addi    1, 1, -16
    li      0, 0
    stw     0, 0(1)
    stw     0, 4(1)

    /* Initialize SysV PPC32 local-exec TLS thread pointer r2 = __tdata_start + 0x7000. */
    lis     2, __tdata_start@h
    ori     2, 2, __tdata_start@l
    addi    2, 2, 0x7000

    /* Zero .tbss and .bss. */
    lis     3, __tbss_start@h
    ori     3, 3, __tbss_start@l
    lis     4, __tbss_end@h
    ori     4, 4, __tbss_end@l
    li      5, 0
1:
    cmplw   0, 3, 4
    bge     2f
    stb     5, 0(3)
    addi    3, 3, 1
    b       1b
2:
    lis     3, __bss_start@h
    ori     3, 3, __bss_start@l
    lis     4, __bss_end@h
    ori     4, 4, __bss_end@l
3:
    cmplw   0, 3, 4
    bge     4f
    stw     5, 0(3)
    addi    3, 3, 4
    b       3b
4:
    /* Call main(0, NULL) and emit low 8-bit exit code on UART1 before DBCR0 reset. */
    li      3, 0
    li      4, 0
    bl      main
    andi.   3, 3, 0xFF
    b       _pir_ppc32_exit

    /* Keep trap handler in .text.boot so its high 16 bits match IVPR. */
    .p2align 4
_pir_ppc32_trap:
    /* PIR trap (tw 31,0,0) -> exit code 132 (matches SIGILL / pirc --interp). */
    li      3, 132
    b       _pir_ppc32_exit

    .text
    /* void _pir_ppc32_exit(int code) -- writes exit code to UART1 and resets QEMU bamboo */
    .globl _pir_ppc32_exit
    .p2align 2
_pir_ppc32_exit:
    lis     4, 0xef600400@h
    ori     4, 4, 0xef600400@l
    stb     3, 0(4)
    sync
    lis     5, 0x3000
    mtspr   308, 5
    isync
1:
    b       1b

    /* void _pir_ppc32_putc(int ch) -- writes 1 byte to bamboo UART0 (stdout) */
    .globl _pir_ppc32_putc
    .p2align 2
_pir_ppc32_putc:
    lis     4, 0xef600300@h
    ori     4, 4, 0xef600300@l
    stb     3, 0(4)
    blr

    /* int __pir_setjmp(void *jb) -- saves lr, sp, cr, r14..r31, f14..f31 (232 bytes) */
    .globl __pir_setjmp
    .p2align 2
__pir_setjmp:
    mflr    0
    stw     0,    0(3)
    stw     1,    4(3)
    mfcr    0
    stw     0,    8(3)
    stw     14,  12(3)
    stw     15,  16(3)
    stw     16,  20(3)
    stw     17,  24(3)
    stw     18,  28(3)
    stw     19,  32(3)
    stw     20,  36(3)
    stw     21,  40(3)
    stw     22,  44(3)
    stw     23,  48(3)
    stw     24,  52(3)
    stw     25,  56(3)
    stw     26,  60(3)
    stw     27,  64(3)
    stw     28,  68(3)
    stw     29,  72(3)
    stw     30,  76(3)
    stw     31,  80(3)
    stfd    14,  88(3)
    stfd    15,  96(3)
    stfd    16, 104(3)
    stfd    17, 112(3)
    stfd    18, 120(3)
    stfd    19, 128(3)
    stfd    20, 136(3)
    stfd    21, 144(3)
    stfd    22, 152(3)
    stfd    23, 160(3)
    stfd    24, 168(3)
    stfd    25, 176(3)
    stfd    26, 184(3)
    stfd    27, 192(3)
    stfd    28, 200(3)
    stfd    29, 208(3)
    stfd    30, 216(3)
    stfd    31, 224(3)
    li      3, 0
    blr

    /* void __pir_longjmp(void *jb, int val) */
    .globl __pir_longjmp
    .p2align 2
__pir_longjmp:
    lwz     0,    0(3)
    mtlr    0
    lwz     1,    4(3)
    lwz     0,    8(3)
    mtcrf   0xff, 0
    lwz     14,  12(3)
    lwz     15,  16(3)
    lwz     16,  20(3)
    lwz     17,  24(3)
    lwz     18,  28(3)
    lwz     19,  32(3)
    lwz     20,  36(3)
    lwz     21,  40(3)
    lwz     22,  44(3)
    lwz     23,  48(3)
    lwz     24,  52(3)
    lwz     25,  56(3)
    lwz     26,  60(3)
    lwz     27,  64(3)
    lwz     28,  68(3)
    lwz     29,  72(3)
    lwz     30,  76(3)
    lwz     31,  80(3)
    lfd     14,  88(3)
    lfd     15,  96(3)
    lfd     16, 104(3)
    lfd     17, 112(3)
    lfd     18, 120(3)
    lfd     19, 128(3)
    lfd     20, 136(3)
    lfd     21, 144(3)
    lfd     22, 152(3)
    lfd     23, 160(3)
    lfd     24, 168(3)
    lfd     25, 176(3)
    lfd     26, 184(3)
    lfd     27, 192(3)
    lfd     28, 200(3)
    lfd     29, 208(3)
    lfd     30, 216(3)
    lfd     31, 224(3)
    cmpwi   0, 4, 0
    bne     1f
    li      4, 1
1:
    mr      3, 4
    blr
