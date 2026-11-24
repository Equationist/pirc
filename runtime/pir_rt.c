/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Minimal native runtime for PIR shadow-stack exceptions (M6+).
 *
 * Canonical frame layout (matches IMPLEMENTATION_PLAN.md Phase A.1 / lower/exceptions.c):
 *   offset 0:               struct PirExcFrame *prev;
 *   offset sizeof(void *):  void *exn;
 *   offset 2*sizeof(void*): jmp_buf jb; (256 bytes reserved on stack)
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct PirExcFrame {
    struct PirExcFrame *prev;
    void               *exn;
    jmp_buf             jb;
} PirExcFrame;

/* Shared TLS shadow-stack head (weak so PIR modules can also emit a weak zeroinit definition). */
#if defined(__APPLE__) || defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) __thread PirExcFrame *__pir_exc_head = NULL;
#else
_Thread_local PirExcFrame *__pir_exc_head = NULL;
#endif

/*
 * __pir_setjmp MUST NOT establish its own stack frame before calling _setjmp,
 * otherwise returning from __pir_setjmp would invalidate the saved SP/LR.
 */
#if defined(__APPLE__) && defined(__aarch64__)
__asm__(
    ".globl ___pir_setjmp\n"
    ".p2align 2\n"
    "___pir_setjmp:\n"
    "    b __setjmp\n"
);
#elif defined(__APPLE__) && defined(__x86_64__)
__asm__(
    ".globl ___pir_setjmp\n"
    "___pir_setjmp:\n"
    "    jmp __setjmp\n"
);
#elif defined(__aarch64__)
__asm__(
    ".globl __pir_setjmp\n"
    ".p2align 2\n"
    "__pir_setjmp:\n"
    "    b _setjmp\n"
);
#elif defined(__x86_64__)
__asm__(
    ".globl __pir_setjmp\n"
    "__pir_setjmp:\n"
    "    jmp _setjmp\n"
);
#endif

void __pir_unwind_resume(void *exn);
void __pir_unwind_resume(void *exn) {
    PirExcFrame *top = __pir_exc_head;
    if (!top) {
        fprintf(stderr, "pir_rt: unhandled exception (%p)\n", exn);
        abort();
    }
    __pir_exc_head = top->prev;
    top->exn = exn;
    _longjmp(top->jb, 1);
}
