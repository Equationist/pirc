/*
 * C17 §6.5.1.1: _Generic selection — exact char/schar/uchar distinction,
 * cv-qualifier stripping, array/function decay, default first/last, nesting,
 * unevaluated non-selected branches, and type-generic macros.
 */
#include "c17_test.h"

#define TYPE_ID(x) _Generic((x), \
    char: 1, \
    signed char: 2, \
    unsigned char: 3, \
    short: 4, \
    int: 5, \
    long: 6, \
    long long: 7, \
    float: 8, \
    double: 9, \
    int *: 10, \
    const int *: 11, \
    int (*)(int): 12, \
    default: 99)

static int sample_fn(int x) { return x + 1; }

static int abs_i(int x) { return x < 0 ? -x : x; }
static double abs_d(double x) { return x < 0.0 ? -x : x; }

#define TG_ABS(x) _Generic((x), \
    int: abs_i, \
    double: abs_d \
)(x)

int main(void) {
    C17_BEGIN("c65_generic_selection", "C17 _Generic: types, lvalue conv, decay, default, unevaluated");

    /* char, signed char, unsigned char are 3 distinct types! */
    char c = 0;
    signed char sc = 0;
    unsigned char uc = 0;
    short sh = 0;
    int i = 0;
    long l = 0;
    long long ll = 0;
    float f = 0.0f;
    double d = 0.0;

    C17_CHECK_EQ(TYPE_ID(c), 1);
    C17_CHECK_EQ(TYPE_ID(sc), 2);
    C17_CHECK_EQ(TYPE_ID(uc), 3);
    C17_CHECK_EQ(TYPE_ID(sh), 4);
    C17_CHECK_EQ(TYPE_ID(i), 5);
    C17_CHECK_EQ(TYPE_ID(l), 6);
    C17_CHECK_EQ(TYPE_ID(ll), 7);
    C17_CHECK_EQ(TYPE_ID(f), 8);
    C17_CHECK_EQ(TYPE_ID(d), 9);

    /* C17 DR 481: lvalue conversion strips top-level const/volatile! */
    const int ci = 10;
    volatile int vi = 20;
    C17_CHECK_EQ(TYPE_ID(ci), 5);
    C17_CHECK_EQ(TYPE_ID(vi), 5);

    /* Array decays to pointer, function designator decays to function pointer */
    int arr[4] = { 1, 2, 3, 4 };
    const int carr[4] = { 1, 2, 3, 4 };
    C17_CHECK_EQ(TYPE_ID(arr), 10);
    C17_CHECK_EQ(TYPE_ID(carr), 11);
    C17_CHECK_EQ(TYPE_ID(sample_fn), 12);

    /* Unmatched type hits default (even when default appears FIRST!) */
    unsigned long ul = 0;
    C17_CHECK_EQ(TYPE_ID(ul), 99);
    int def_first = _Generic(ul, default: 77, int: 1, double: 2);
    C17_CHECK_EQ(def_first, 77);

    /* Controlling expression AND non-selected branches are NOT evaluated! */
    int side = 0;
    int chosen = _Generic(++side, int: 42, double: (++side, 100));
    C17_CHECK_EQ(chosen, 42);
    C17_CHECK_EQ(side, 0);

    /* Type-generic function dispatch macro */
    C17_CHECK_EQ(TG_ABS(-17), 17);
    C17_CHECK_DBL(TG_ABS(-3.5), 3.5, 0.0);

    /* Nested _Generic */
    int nested = _Generic(i, int: _Generic(d, double: 123, default: 0), default: -1);
    C17_CHECK_EQ(nested, 123);

    C17_END();
}
