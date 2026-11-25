/* Cross-compiler ABI interop: slimcc-pir TU calling into host clang TU */
#include "c17_test.h"
#include "interop.h"

static int slimcc_callback(int a, double b) {
    return a * 10 + (int)(b * 2.0);
}

int main(void) {
    C99_BEGIN("l67_pos_clang_interop", "ABI interop between slimcc-pir TU and host clang TU");

    struct InteropHFA h = clang_add_hfa((struct InteropHFA){ 1.5, 2.5, 3.5 },
                                        (struct InteropHFA){ 0.5, 1.5, 2.5 });
    C99_CHECK_DBL(h.x, 2.0, 0.0);
    C99_CHECK_DBL(h.y, 4.0, 0.0);
    C99_CHECK_DBL(h.z, 6.0, 0.0);

    struct InteropLarge l = clang_sum_large((struct InteropLarge){ { 1, 2, 3, 4, 5 } },
                                            (struct InteropLarge){ { 10, 20, 30, 40, 50 } });
    C99_CHECK_EQ(l.v[0], 11);
    C99_CHECK_EQ(l.v[4], 55);

    C99_CHECK_EQ(clang_negate_bool(true), 0);
    C99_CHECK_EQ(clang_negate_bool(false), 1);

    long long vm = clang_varargs_mix(4, 10, 20LL, 30, 40LL);
    C99_CHECK_EQ(vm, 100);

    C99_CHECK_EQ(clang_call_cb(slimcc_callback, 7, 3.5), 77);

    C99_END();
}
