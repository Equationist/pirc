/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Minimal bare-metal RISC-V 32-bit startup, trap handler, semihosting I/O/exit,
 * and setjmp/longjmp for QEMU virt (-machine virt -bios none -semihosting).
 */

    .section .text.boot,"ax",@progbits
    .globl _start
    .p2align 2
_start:
    /* Enable FPU (mstatus.FS = Dirty = 0x6000) so RV32FD instructions do not trap. */
    li      t0, 0x6000
    csrs    mstatus, t0

    /* Install machine trap handler (PIR trap / unimp exits with code 132). */
    lla     t0, _pir_rv32_trap
    csrw    mtvec, t0

    /* Initialize 16-byte aligned stack pointer. */
    lla     sp, _stack_top

    /* Initialize thread pointer tp to .tdata start for local-exec TLS. */
    lla     tp, __tdata_start

    /* Zero .tbss and .bss. */
    lla     t0, __tbss_start
    lla     t1, __tbss_end
1:
    bgeu    t0, t1, 2f
    sb      zero, 0(t0)
    addi    t0, t0, 1
    j       1b
2:
    lla     t0, __bss_start
    lla     t1, __bss_end
3:
    bgeu    t0, t1, 4f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       3b
4:
    /* Call main(0, NULL) and exit QEMU with its low 8-bit exit code. */
    li      a0, 0
    li      a1, 0
    call    main
    andi    a0, a0, 0xFF
    j       _pir_rv32_exit

    .text
    .p2align 2
_pir_rv32_trap:
    /* PIR trap (unimp) -> exit code 132 (matches SIGILL / pirc --interp). */
    li      a0, 132
    j       _pir_rv32_exit

    /* void _pir_rv32_exit(int code) -- exits QEMU via RISC-V semihosting SYS_EXIT_EXTENDED */
    .globl _pir_rv32_exit
    .p2align 2
_pir_rv32_exit:
    addi    sp, sp, -16
    li      t0, 0x20026             /* ADP_Stopped_ApplicationExit */
    sw      t0, 0(sp)
    sw      a0, 4(sp)               /* subcode = host exit code */
    li      a0, 0x20                /* SYS_EXIT_EXTENDED */
    mv      a1, sp
    .option push
    .option norvc
    slli    zero, zero, 0x1f
    ebreak
    srai    zero, zero, 0x7
    .option pop
    /* Fallback: SiFive virt test finisher at 0x100000 */
    li      t0, 0x100000
    lw      t1, 4(sp)
    beqz    t1, 1f
    slli    t1, t1, 16
    li      t2, 0x3333              /* FINISHER_FAIL | (code << 16) */
    or      t1, t1, t2
    sw      t1, 0(t0)
    j       2f
1:
    li      t1, 0x5555              /* FINISHER_PASS */
    sw      t1, 0(t0)
2:
    j       2b

    /* void _pir_rv32_putc(int ch) -- writes 1 byte to QEMU virt NS16550A UART0 (stdout) */
    .globl _pir_rv32_putc
    .p2align 2
_pir_rv32_putc:
    li      t0, 0x10000000
    sb      a0, 0(t0)
    ret

    /* int __pir_setjmp(void *jb) -- saves ra, sp, s0..s11, fs0..fs11 (152 bytes) */
    .globl __pir_setjmp
    .p2align 2
__pir_setjmp:
    sw      ra,   0(a0)
    sw      sp,   4(a0)
    sw      s0,   8(a0)
    sw      s1,  12(a0)
    sw      s2,  16(a0)
    sw      s3,  20(a0)
    sw      s4,  24(a0)
    sw      s5,  28(a0)
    sw      s6,  32(a0)
    sw      s7,  36(a0)
    sw      s8,  40(a0)
    sw      s9,  44(a0)
    sw      s10, 48(a0)
    sw      s11, 52(a0)
    fsd     fs0,  56(a0)
    fsd     fs1,  64(a0)
    fsd     fs2,  72(a0)
    fsd     fs3,  80(a0)
    fsd     fs4,  88(a0)
    fsd     fs5,  96(a0)
    fsd     fs6, 104(a0)
    fsd     fs7, 112(a0)
    fsd     fs8, 120(a0)
    fsd     fs9, 128(a0)
    fsd     fs10,136(a0)
    fsd     fs11,144(a0)
    li      a0, 0
    ret

    /* void __pir_longjmp(void *jb, int val) */
    .globl __pir_longjmp
    .p2align 2
__pir_longjmp:
    lw      ra,   0(a0)
    lw      sp,   4(a0)
    lw      s0,   8(a0)
    lw      s1,  12(a0)
    lw      s2,  16(a0)
    lw      s3,  20(a0)
    lw      s4,  24(a0)
    lw      s5,  28(a0)
    lw      s6,  32(a0)
    lw      s7,  36(a0)
    lw      s8,  40(a0)
    lw      s9,  44(a0)
    lw      s10, 48(a0)
    lw      s11, 52(a0)
    fld     fs0,  56(a0)
    fld     fs1,  64(a0)
    fld     fs2,  72(a0)
    fld     fs3,  80(a0)
    fld     fs4,  88(a0)
    fld     fs5,  96(a0)
    fld     fs6, 104(a0)
    fld     fs7, 112(a0)
    fld     fs8, 120(a0)
    fld     fs9, 128(a0)
    fld     fs10,136(a0)
    fld     fs11,144(a0)
    bnez    a1, 1f
    li      a1, 1
1:
    mv      a0, a1
    ret
