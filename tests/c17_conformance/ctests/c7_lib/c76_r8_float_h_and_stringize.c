/* ISO C17 regression tests (Round 8):
 * - <float.h> LDBL_* / DECIMAL_DIG / FLT_ROUNDS consistency with long double
 * - C17 §6.10.3.2p2 stringizing standalone backslash tokens
 * - Uninitialized & VLA declarations in `for` loop headers
 * - Qualified pointer-to-array ==, <, and subtraction
 * - Block-scope compound literal in unevaluated sizeof / _Generic inside static init
 * - Array parameter [_Atomic N] vs [N] redeclaration compatibility
 * - Block-scope VLA function prototype lifetime safety (ASan UAF guard)
 * - >32 distinct struct types (pir_types realloc UAF guard)
 */
#include "c17_test.h"
#include <float.h>
#include <math.h>
#include <stddef.h>

#define STRZ(x) #x

void r8_arr_atomic_param(int a[_Atomic 10]);
void r8_arr_atomic_param(int a[10]) { a[0] += 1; }

void r8_vla_proto_fn(int (*p)[]);
static void r8_decl_block_vla_proto(int n) {
    void r8_vla_proto_fn(int (*p)[n]);
    (void)n;
}
void r8_vla_proto_fn(int (*p)[]) { (void)p; }

static long r8_ptr_arr_cmp_sub(int (*p)[5], const int (*q)[5]) {
    return (long)(p == q) + (long)(p < q) * 2L + (p - q) * 10L;
}

static int r8_static_uneval_complit(int g) {
    static int s_sz = (int)sizeof((int){ g });
    static int s_gen = _Generic((int){ 1 }, int: 11, default: 0);
    return s_sz + s_gen;
}

#define DEF_S(N) \
    struct R8S_##N { unsigned char a[N]; int tag; }; \
    static struct R8S_##N make_r8s_##N(int t) { \
        struct R8S_##N s = { { (unsigned char)(N) }, t }; \
        return s; \
    }

DEF_S(1)  DEF_S(2)  DEF_S(3)  DEF_S(4)  DEF_S(5)  DEF_S(6)  DEF_S(7)  DEF_S(8)
DEF_S(9)  DEF_S(10) DEF_S(11) DEF_S(12) DEF_S(13) DEF_S(14) DEF_S(15) DEF_S(16)
DEF_S(17) DEF_S(18) DEF_S(19) DEF_S(20) DEF_S(21) DEF_S(22) DEF_S(23) DEF_S(24)
DEF_S(25) DEF_S(26) DEF_S(27) DEF_S(28) DEF_S(29) DEF_S(30) DEF_S(31) DEF_S(32)
DEF_S(33) DEF_S(34) DEF_S(35) DEF_S(36)

int main(void) {
    /* 1. <float.h> checks */
    C17_CHECK(FLT_ROUNDS >= -1);
    C17_CHECK(isfinite(LDBL_MAX));
    C17_CHECK(LDBL_MIN > 0.0L);
    C17_CHECK(LDBL_TRUE_MIN > 0.0L);
    C17_CHECK(1.0L + LDBL_EPSILON > 1.0L);
    if (sizeof(long double) == sizeof(double)) {
        C17_CHECK(LDBL_MANT_DIG == DBL_MANT_DIG);
        C17_CHECK(LDBL_DIG == DBL_DIG);
        C17_CHECK(LDBL_MAX_EXP == DBL_MAX_EXP);
        C17_CHECK(LDBL_MIN_EXP == DBL_MIN_EXP);
        C17_CHECK(LDBL_DECIMAL_DIG == DBL_DECIMAL_DIG);
        C17_CHECK(DECIMAL_DIG >= DBL_DECIMAL_DIG);
    }

    /* 2. Stringizing standalone backslash tokens (C17 6.10.3.2p2) */
    _Static_assert(sizeof(STRZ(\\)) == 2, "C17 6.10.3.2p2");
    C17_CHECK(STRZ(\\)[0] == '\\' && STRZ(\\)[1] == '\0');

    /* 3. for-header uninitialized & VLA declarations */
    int for_sum = 0;
    for (int i; ; ) {
        i = 7;
        for_sum += i;
        break;
    }
    int n = 4;
    for (int a[n]; ; ) {
        a[0] = 9;
        a[n - 1] = 13;
        for_sum += a[0] + a[n - 1];
        break;
    }
    C17_CHECK(for_sum == 29);

    /* 4. Qualified pointer-to-array comparisons & subtraction */
    int mat[3][5] = { {1}, {2}, {3} };
    C17_CHECK(r8_ptr_arr_cmp_sub(&mat[0], (const int (*)[5])&mat[0]) == 1L);
    C17_CHECK(r8_ptr_arr_cmp_sub(&mat[0], (const int (*)[5])&mat[2]) == -18L);
    C17_CHECK(r8_ptr_arr_cmp_sub(&mat[2], (const int (*)[5])&mat[0]) == 20L);

    /* 5. Unevaluated compound literal in static local initializer */
    C17_CHECK(r8_static_uneval_complit(123) == (int)sizeof(int) + 11);

    /* 6. Array param [_Atomic N] & block-scope VLA prototype */
    int arr10[10] = { 41 };
    r8_arr_atomic_param(arr10);
    C17_CHECK(arr10[0] == 42);
    r8_decl_block_vla_proto(5);
    r8_vla_proto_fn(&mat[0]);

    /* 7. >32 distinct PIR aggregate types */
    int ssum = 0;
    ssum += make_r8s_1(1).tag + make_r8s_2(2).tag + make_r8s_3(3).tag + make_r8s_4(4).tag;
    ssum += make_r8s_5(5).tag + make_r8s_6(6).tag + make_r8s_7(7).tag + make_r8s_8(8).tag;
    ssum += make_r8s_9(9).tag + make_r8s_10(10).tag + make_r8s_11(11).tag + make_r8s_12(12).tag;
    ssum += make_r8s_13(13).tag + make_r8s_14(14).tag + make_r8s_15(15).tag + make_r8s_16(16).tag;
    ssum += make_r8s_17(17).tag + make_r8s_18(18).tag + make_r8s_19(19).tag + make_r8s_20(20).tag;
    ssum += make_r8s_21(21).tag + make_r8s_22(22).tag + make_r8s_23(23).tag + make_r8s_24(24).tag;
    ssum += make_r8s_25(25).tag + make_r8s_26(26).tag + make_r8s_27(27).tag + make_r8s_28(28).tag;
    ssum += make_r8s_29(29).tag + make_r8s_30(30).tag + make_r8s_31(31).tag + make_r8s_32(32).tag;
    ssum += make_r8s_33(33).tag + make_r8s_34(34).tag + make_r8s_35(35).tag + make_r8s_36(36).tag;
    C17_CHECK(ssum == 666);

    C17_END();
}
