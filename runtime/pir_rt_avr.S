; Copyright (c) 2026 Ummon Karpe
; SPDX-License-Identifier: BSD-3-Clause
;
; Bare-metal ATmega1280 startup, dual-UART I/O, trap, and setjmp/longjmp
; for QEMU (-M mega -serial file:stdout -serial file:exitcode).

    .section .vectors,"ax",@progbits
    .globl _vectors
_vectors:
    jmp _start

    .section .text.startup,"ax",@progbits
    .globl _start
_start:
    ; Establish AVR __zero_reg__ (r1 == 0) and clean SREG
    clr r1
    out 0x3f, r1

    ; Initialize stack pointer to top of 8 KiB SRAM (0x21FF)
    ldi r28, lo8(__stack_top)
    ldi r29, hi8(__stack_top)
    out 0x3d, r28
    out 0x3e, r29

    ; Copy .data + .rodata + .tdata from Flash (24-bit via RAMPZ:Z) to SRAM (X)
    ldi r30, lo8(__data_load_start)
    ldi r31, hi8(__data_load_start)
    ldi r24, hh8(__data_load_start)
    out 0x3b, r24
    ldi r26, lo8(__data_start)
    ldi r27, hi8(__data_start)
    ldi r22, lo8(__data_end)
    ldi r23, hi8(__data_end)
1:
    cp  r26, r22
    cpc r27, r23
    brsh 2f
    elpm r0, Z+
    st  X+, r0
    rjmp 1b
2:

    ; Zero .bss + .tbss in SRAM
    ldi r26, lo8(__bss_start)
    ldi r27, hi8(__bss_start)
    ldi r22, lo8(__bss_end)
    ldi r23, hi8(__bss_end)
4:
    cp  r26, r22
    cpc r27, r23
    brsh 5f
    st  X+, r1
    rjmp 4b
5:

    ; Enable USART0 TX (UCSR0B @ 0xC1) for stdout and USART1 TX (UCSR1B @ 0xC9) for exit code
    ldi r24, 0x08
    sts 0x00c1, r24
    sts 0x00c9, r24

#ifdef PIR_AVR_MAIN_I32_SRET
    ; PIR_CC_PIR @main() -> i32 lowered by i32.c to @main(%out: ptr) -> void:
    ; Reserve 4-byte return buffer on stack and pass its address in r24:r25.
    push r1
    push r1
    push r1
    push r1
    in r24, 0x3d
    in r25, 0x3e
    adiw r24, 1
    call main
    pop r24
    rjmp _pir_avr_exit
#elif defined(PIR_AVR_MAIN_I32)
    ; PIR_CC_C @main() -> i32 returns low word in r22:r23, high word in r24:r25.
    call main
    mov r24, r22
    rjmp _pir_avr_exit
#else
    ; Native AVR @main() -> i16 returns exit code in r24:r25.
    call main
    rjmp _pir_avr_exit
#endif

    .text
    .globl _pir_avr_putc
_pir_avr_putc:
    sts 0x00c6, r24
    ret

    .globl _pir_avr_exit
_pir_avr_exit:
    sts 0x00ce, r24
1:
    rjmp 1b

    .globl __pir_avr_trap
__pir_avr_trap:
    ldi r24, 132
    rjmp _pir_avr_exit

; int32_t __pir_setjmp(void *jb)
;   jb in r24:r25 -> saves r2..r17 (16B), r28:r29 (2B), SP (2B), ret_addr bytes (2B) = 22B
;   returns i32 0 in r22:r25
    .globl __pir_setjmp
__pir_setjmp:
    movw r30, r24
    st Z+, r2
    st Z+, r3
    st Z+, r4
    st Z+, r5
    st Z+, r6
    st Z+, r7
    st Z+, r8
    st Z+, r9
    st Z+, r10
    st Z+, r11
    st Z+, r12
    st Z+, r13
    st Z+, r14
    st Z+, r15
    st Z+, r16
    st Z+, r17
    st Z+, r28
    st Z+, r29
    ; Save current SP (points right below the 2-byte return address on stack)
    in r26, 0x3d
    in r27, 0x3e
    st Z+, r26
    st Z+, r27
    ; Save exact 2 return-address bytes at [SP+1] and [SP+2]
    adiw r26, 1
    ld r0, X+
    st Z+, r0
    ld r0, X
    st Z+, r0
    ; Return i32 0 in r22:r25
    clr r22
    clr r23
    clr r24
    clr r25
    ret

; void __pir_longjmp(void *jb, int16_t val)
;   jb in r24:r25, val in r22:r23 -> restores callee-saved regs, Y, SP, ret_addr,
;   returns non-zero i32 (1) in r22:r25
    .globl __pir_longjmp
__pir_longjmp:
    movw r30, r24
    ld r2,  Z+
    ld r3,  Z+
    ld r4,  Z+
    ld r5,  Z+
    ld r6,  Z+
    ld r7,  Z+
    ld r8,  Z+
    ld r9,  Z+
    ld r10, Z+
    ld r11, Z+
    ld r12, Z+
    ld r13, Z+
    ld r14, Z+
    ld r15, Z+
    ld r16, Z+
    ld r17, Z+
    ld r28, Z+
    ld r29, Z+
    ; Restore SP (interrupt-safe)
    ld r26, Z+
    ld r27, Z+
    in r0, 0x3f
    cli
    out 0x3e, r27
    out 0x3f, r0
    out 0x3d, r26
    ; Rewrite exact 2 return-address bytes at [SP+1] and [SP+2]
    adiw r26, 1
    ld r0, Z+
    st X+, r0
    ld r0, Z+
    st X,  r0
    ; Ensure __zero_reg__ == 0 and return i32 1 in r22:r25
    clr r1
    ldi r22, 1
    clr r23
    clr r24
    clr r25
    ret
