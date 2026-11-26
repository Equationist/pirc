#include "interop.h"

struct Mixed12 clang_ret_mixed12(int64_t a, float b) {
    struct Mixed12 r;
    r.a = a;
    r.b = b;
    return r;
}

struct Fp12 clang_ret_fp12(double a, float b) {
    struct Fp12 r;
    r.a = a;
    r.b = b;
    return r;
}

int clang_stack_signext(int a1, int a2, int a3, int a4, int a5, int a6,
                        signed char s7, short s8, signed char s9) {
    return a1 + a2 + a3 + a4 + a5 + a6 + (int)s7 + (int)s8 + (int)s9;
}

int clang_check_aligned16(int a1, struct Aligned16 s, int a2) {
    return a1 + s.a + s.b + s.c + s.d + a2;
}

int clang_call_pir_funcs(void) {
    struct Mixed12 m = pir_ret_mixed12(-987654321LL, 6.25f);
    if (m.a != -987654321LL || m.b != 6.25f) return 1;

    struct Fp12 f = pir_ret_fp12(-4.5, 9.75f);
    if (f.a != -4.5 || f.b != 9.75f) return 2;

    int sres = pir_stack_signext(10, 20, 30, 40, 50, 60, (signed char)-1, (short)-200, (signed char)-9);
    if (sres != (210 - 1 - 200 - 9)) return 3;

    return 0;
}
