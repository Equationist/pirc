/* Bidirectional clang <-> slimcc-pir Round 5 ABI interop:
 * - x64 partial trailing FP multi-reg return (Mixed12, Fp12)
 * - x64 7th+ stack GP argument signext (signed char, short)
 * - _Alignas(16) struct parameter passing */
#include "c17_test.h"
#include "interop.h"

struct Mixed12 pir_ret_mixed12(int64_t a, float b) {
    struct Mixed12 r;
    r.a = a;
    r.b = b;
    return r;
}

struct Fp12 pir_ret_fp12(double a, float b) {
    struct Fp12 r;
    r.a = a;
    r.b = b;
    return r;
}

int pir_stack_signext(int a1, int a2, int a3, int a4, int a5, int a6,
                      signed char s7, short s8, signed char s9) {
    return a1 + a2 + a3 + a4 + a5 + a6 + (int)s7 + (int)s8 + (int)s9;
}

int main(void) {
    /* 1. slimcc-pir calling clang: partial trailing FP multi-reg return */
    struct Mixed12 m = clang_ret_mixed12(123456789LL, 3.5f);
    C17_CHECK(m.a == 123456789LL);
    C17_CHECK(m.b == 3.5f);

    struct Fp12 f = clang_ret_fp12(2.25, -7.5f);
    C17_CHECK(f.a == 2.25);
    C17_CHECK(f.b == -7.5f);

    /* 2. slimcc-pir calling clang: 7th+ stack GP signext */
    int sres = clang_stack_signext(1, 2, 3, 4, 5, 6, (signed char)-5, (short)-300, (signed char)-12);
    C17_CHECK(sres == (21 - 5 - 300 - 12));

    /* 3. slimcc-pir calling clang: _Alignas(16) struct */
    struct Aligned16 al = {10, 20, 30, 40};
    C17_CHECK(clang_check_aligned16(7, al, 13) == 120);

    /* 4. clang calling slimcc-pir */
    C17_CHECK(clang_call_pir_funcs() == 0);

    C17_END();
}
