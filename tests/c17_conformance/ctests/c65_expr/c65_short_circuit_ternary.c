/*
 * C99 §6.5.13-§6.5.17: Short-circuit && / ||, ternary ?: (void, qualified
 * pointers, null pointer constants, structs, right-associativity), and comma.
 */
#include "c17_test.h"

static int log_buf[16];
static int log_len = 0;

static int record(int id, int ret) {
    log_buf[log_len++] = id;
    return ret;
}

static void vrecord(int id) {
    log_buf[log_len++] = id;
}

struct Pair {
    int a;
    int b;
};

int main(void) {
    C99_BEGIN("c65_short_circuit_ternary", "Short-circuit &&/||, ternary ?:, comma operator");

    /* && short-circuit and result normalization to 0 or 1 */
    log_len = 0;
    int r1 = record(1, 0) && record(2, 1);
    C99_CHECK_EQ(r1, 0);
    C99_CHECK_EQ(log_len, 1);
    C99_CHECK_EQ(log_buf[0], 1);

    log_len = 0;
    int r2 = record(10, 42) && record(20, -7);
    C99_CHECK_EQ(r2, 1); /* Must be 1, not -7 */
    C99_CHECK_EQ(log_len, 2);

    /* || short-circuit and result normalization */
    log_len = 0;
    int r3 = record(3, 99) || record(4, 0);
    C99_CHECK_EQ(r3, 1);
    C99_CHECK_EQ(log_len, 1);

    /* Float/pointer operands to &&, ||, ! */
    double d_tiny = c99_opaque_dbl(0.25);
    double d_zero = c99_opaque_dbl(0.0);
    C99_CHECK_EQ(d_tiny && !d_zero, 1);
    C99_CHECK_EQ(!d_tiny, 0);
    C99_CHECK_EQ(!d_zero, 1);

    /* Ternary evaluates ONLY chosen branch */
    log_len = 0;
    int t1 = c99_opaque_int(1) ? record(5, 111) : record(6, 222);
    C99_CHECK_EQ(t1, 111);
    C99_CHECK_EQ(log_len, 1);
    C99_CHECK_EQ(log_buf[0], 5);

    /* Ternary with void branches */
    log_len = 0;
    c99_opaque_int(0) ? vrecord(7) : vrecord(8);
    C99_CHECK_EQ(log_len, 1);
    C99_CHECK_EQ(log_buf[0], 8);

    /* Ternary right-associativity: a ? b : c ? d : e == a ? b : (c ? d : e) */
    int k = c99_opaque_int(2);
    int t2 = (k == 1) ? 10 : (k == 2) ? 20 : 30;
    C99_CHECK_EQ(t2, 20);

    /* Ternary combining qualified pointers & null pointer constant (§6.5.15 p6) */
    int val = 77;
    const int cval = 88;
    int *p_mut = &val;
    const int *p_const = &cval;
    const int *p_sel = c99_opaque_int(1) ? p_mut : p_const;
    C99_CHECK_EQ(*p_sel, 77);
    const int *p_null = c99_opaque_int(0) ? p_mut : (void *)0;
    C99_CHECK(p_null == NULL);

    /* Ternary returning struct by value */
    struct Pair p1 = { 1, 2 };
    struct Pair p2 = { 3, 4 };
    struct Pair p3 = c99_opaque_int(0) ? p1 : p2;
    C99_CHECK_EQ(p3.a, 3);
    C99_CHECK_EQ(p3.b, 4);

    /* Comma operator left-to-right sequencing */
    int a = 1;
    int b = (a += 2, a *= 4, a + 1);
    C99_CHECK_EQ(a, 12);
    C99_CHECK_EQ(b, 13);

    C99_END();
}
