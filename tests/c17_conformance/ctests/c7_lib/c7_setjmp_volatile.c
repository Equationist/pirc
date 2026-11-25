/*
 * C99 §7.13: <setjmp.h> — setjmp/longjmp, longjmp(env, 0) returning 1,
 * volatile local variable preservation across longjmp, and nested setjmp.
 */
#include "c17_test.h"
#include <setjmp.h>

static jmp_buf env_outer;
static jmp_buf env_inner;

static void jump_deep(jmp_buf *jb, int val) {
    longjmp(*jb, val);
}

int main(void) {
    C99_BEGIN("c7_setjmp_volatile", "setjmp/longjmp: zero->1 rule, volatile locals, nesting");

    /* Basic setjmp + volatile local modification across longjmp */
    volatile int state = 10;
    int r = setjmp(env_outer);
    if (r == 0) {
        state = 42;
        jump_deep(&env_outer, 7);
        C99_FAIL("unreachable after longjmp");
    }
    C99_CHECK_EQ(r, 7);
    C99_CHECK_EQ(state, 42);

    /* §7.13.2.1 p4: longjmp(env, 0) MUST cause setjmp to return 1! */
    volatile int got_zero_case = 0;
    int r0 = setjmp(env_outer);
    if (r0 == 0) {
        got_zero_case = 1;
        longjmp(env_outer, 0);
    }
    C99_CHECK_EQ(got_zero_case, 1);
    C99_CHECK_EQ(r0, 1);

    /* Nested setjmp buffers */
    volatile int step = 0;
    if (setjmp(env_outer) == 0) {
        step = 1;
        if (setjmp(env_inner) == 0) {
            step = 2;
            longjmp(env_inner, 11);
        } else {
            step = 3;
            longjmp(env_outer, 22);
        }
    }
    C99_CHECK_EQ(step, 3);

    C99_END();
}
