@ Copyright (c) 2026 Ummon Karpe
@ SPDX-License-Identifier: BSD-3-Clause
@
@ Bare-metal Cortex-M7 / ARMv7E-M Thumb-2 startup, ARM semihosting I/O & exit,
@ fault trap handler, and setjmp/longjmp for QEMU -M mps2-an500.

	.syntax unified
	.thumb

	.section .vectors, "a", %progbits
	.p2align 2
	.globl __vectors
__vectors:
	.word __stack_top
	.word _start
	.word __pir_t2_trap   @ NMI
	.word __pir_t2_trap   @ HardFault
	.word __pir_t2_trap   @ MemManage
	.word __pir_t2_trap   @ BusFault
	.word __pir_t2_trap   @ UsageFault
	.word 0
	.word 0
	.word 0
	.word 0
	.word __pir_t2_trap   @ SVCall
	.word __pir_t2_trap   @ DebugMon
	.word 0
	.word __pir_t2_trap   @ PendSV
	.word __pir_t2_trap   @ SysTick

	.text
	.p2align 1
	.globl _start
	.type _start, %function
	.thumb_func
_start:
	@ Enable FPU CP10 & CP11 full access in CPACR (0xE000ED88)
	movw r0, #:lower16:0xE000ED88
	movt r0, #:upper16:0xE000ED88
	ldr r1, [r0]
	orr r1, r1, #(0xF << 20)
	str r1, [r0]
	dsb
	isb

	@ Copy .data from Flash (__data_load) to RAM (__data_start..__data_end)
	movw r0, #:lower16:__data_start
	movt r0, #:upper16:__data_start
	movw r1, #:lower16:__data_end
	movt r1, #:upper16:__data_end
	movw r2, #:lower16:__data_load
	movt r2, #:upper16:__data_load
1:
	cmp r0, r1
	bhs 2f
	ldr r3, [r2], #4
	str r3, [r0], #4
	b 1b
2:

	@ Clear .bss (__bss_start..__bss_end)
	movw r0, #:lower16:__bss_start
	movt r0, #:upper16:__bss_start
	movw r1, #:lower16:__bss_end
	movt r1, #:upper16:__bss_end
	movs r2, #0
3:
	cmp r0, r1
	bhs 4f
	str r2, [r0], #4
	b 3b
4:

	@ Call main(0, NULL)
	movs r0, #0
	movs r1, #0
	bl main
	and r0, r0, #0xFF
	bl __pir_t2_exit
5:
	b 5b
	.size _start, .-_start

	.p2align 1
	.globl __pir_t2_exit
	.type __pir_t2_exit, %function
	.thumb_func
__pir_t2_exit:
	@ ARM semihosting SYS_EXIT_EXTENDED (0x20): block = { ADP_Stopped_ApplicationExit, code }
	sub sp, sp, #8
	movw r2, #0x0026
	movt r2, #0x0002
	str r2, [sp, #0]
	str r0, [sp, #4]
	movs r0, #0x20
	mov r1, sp
	bkpt #0xAB
1:
	b 1b
	.size __pir_t2_exit, .-__pir_t2_exit

	.p2align 1
	.globl __pir_t2_trap
	.type __pir_t2_trap, %function
	.thumb_func
__pir_t2_trap:
	movs r0, #132
	b __pir_t2_exit
	.size __pir_t2_trap, .-__pir_t2_trap

	.p2align 1
	.globl __pir_t2_putc
	.type __pir_t2_putc, %function
	.thumb_func
__pir_t2_putc:
	@ ARM semihosting SYS_WRITEC (0x03): r1 = pointer to byte
	sub sp, sp, #8
	strb r0, [sp, #0]
	movs r0, #0x03
	mov r1, sp
	bkpt #0xAB
	add sp, sp, #8
	bx lr
	.size __pir_t2_putc, .-__pir_t2_putc

	.p2align 1
	.globl __pir_setjmp
	.type __pir_setjmp, %function
	.thumb_func
__pir_setjmp:
	@ Save r4..r11, sp, lr (40 bytes) + d8..d15 (64 bytes if hard-float)
	stmia r0!, {r4, r5, r6, r7, r8, r9, r10, r11}
	mov r2, sp
	mov r3, lr
	stmia r0!, {r2, r3}
#ifdef __ARM_FP
	vstmia.64 r0!, {d8-d15}
#endif
	movs r0, #0
	bx lr
	.size __pir_setjmp, .-__pir_setjmp

	.p2align 1
	.globl __pir_longjmp
	.type __pir_longjmp, %function
	.thumb_func
__pir_longjmp:
	ldmia r0!, {r4, r5, r6, r7, r8, r9, r10, r11}
	ldmia r0!, {r2, r3}
	mov sp, r2
	mov lr, r3
#ifdef __ARM_FP
	vldmia.64 r0!, {d8-d15}
#endif
	cmp r1, #0
	it eq
	moveq r1, #1
	mov r0, r1
	bx lr
	.size __pir_longjmp, .-__pir_longjmp
