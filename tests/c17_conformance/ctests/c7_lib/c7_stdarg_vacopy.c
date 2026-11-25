/*
 * C99 §7.15: <stdarg.h> — va_start, va_arg, va_end, C99 va_copy, passing
 * va_list to helper functions, mixed scalar/double/pointer/struct varargs,
 * and >12 arguments (testing both register & stack spills).
 */
#include "c17_test.h"
#include <stdarg.h>

struct SmallPair {
    int x;
    int y;
};

static long long vsum_ints(int n, va_list ap) {
    long long s = 0;
    for (int i = 0; i < n; i++) {
        s += va_arg(ap, int);
    }
    return s;
}

static long long sum_with_vacopy(int n, ...) {
    va_list ap1, ap2;
    va_start(ap1, n);
    va_copy(ap2, ap1);
    long long s1 = vsum_ints(n, ap1);
    long long s2 = vsum_ints(n, ap2);
    va_end(ap2);
    va_end(ap1);
    return (s1 == s2) ? s1 : -1;
}

static double mixed_varargs(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    double total = 0.0;
    for (const char *p = fmt; *p; p++) {
        if (*p == 'i') {
            total += (double)va_arg(ap, int);
        } else if (*p == 'l') {
            total += (double)va_arg(ap, long long);
        } else if (*p == 'd') {
            total += va_arg(ap, double);
        } else if (*p == 's') {
            const char *str = va_arg(ap, const char *);
            total += (double)strlen(str);
        } else if (*p == 'p') {
            struct SmallPair sp = va_arg(ap, struct SmallPair);
            total += (double)(sp.x + sp.y);
        }
    }
    va_end(ap);
    return total;
}

int main(void) {
    C99_BEGIN("c7_stdarg_vacopy", "stdarg: va_copy, va_list helpers, mixed & >12 varargs");

    /* 14 variadic int args + va_copy verifying exact replay */
    long long s14 = sum_with_vacopy(14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14);
    C99_CHECK_EQ(s14, 105);

    /* Mixed int, double (and promoted float!), long long, char*, struct */
    struct SmallPair sp = { 17, 23 };
    float f = 2.5f; /* Default argument promotion promotes float -> double! */
    double res = mixed_varargs("idlsipd",
                               10,        /* i: 10 */
                               f,         /* d: 2.5 */
                               100LL,     /* l: 100 */
                               "hello",   /* s: 5 */
                               -7,        /* i: -7 */
                               sp,        /* p: 40 */
                               0.5);      /* d: 0.5 */
    C99_CHECK_DBL(res, 151.0, 0.0);

    C99_END();
}
