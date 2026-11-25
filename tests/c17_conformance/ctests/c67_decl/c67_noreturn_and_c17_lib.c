/*
 * C17 §6.7.4 (_Noreturn), §7.22.3.1 (aligned_alloc), §7.27.2.5 (timespec_get),
 * and §6.10.8 (__STDC_VERSION__ == 201710L).
 */
#include "c17_test.h"
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <time.h>

static jmp_buf nr_env;

_Noreturn static void escape_raw(int code) {
    longjmp(nr_env, code);
}

noreturn static void escape_macro(int code) {
    longjmp(nr_env, code);
}

int main(void) {
    C17_BEGIN("c67_noreturn_and_c17_lib", "C17 _Noreturn, aligned_alloc, timespec_get, version macro");

    C17_CHECK_EQ(__STDC_VERSION__, 201710L);

    /* _Noreturn and noreturn functions */
    int r1 = setjmp(nr_env);
    if (r1 == 0) {
        escape_raw(17);
    }
    C17_CHECK_EQ(r1, 17);

    int r2 = setjmp(nr_env);
    if (r2 == 0) {
        escape_macro(29);
    }
    C17_CHECK_EQ(r2, 29);

    /* C11/C17 aligned_alloc */
    void *p = aligned_alloc(64, 256);
    C17_CHECK(p != NULL);
    C17_CHECK_EQ((uintptr_t)p % 64, 0);
    memset(p, 0xA5, 256);
    C17_CHECK_EQ(((unsigned char *)p)[255], 0xA5);
    free(p);

    /* C11/C17 timespec_get */
    struct timespec ts = { 0, 0 };
    int base = timespec_get(&ts, TIME_UTC);
    C17_CHECK_EQ(base, TIME_UTC);
    C17_CHECK(ts.tv_sec > 0);
    C17_CHECK(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000L);

    C17_END();
}
