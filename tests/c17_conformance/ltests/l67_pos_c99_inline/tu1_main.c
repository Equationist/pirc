/* C99 §6.7.4 p6: C99 inline linkage across TUs (TU 1 - consumer) */
#include "c17_test.h"
#include "inline_hdr.h"

extern int call_from_tu2(int x, int y);

int main(void) {
    C99_BEGIN("l67_pos_c99_inline", "C99 inline + extern inline external definition across TUs");
    C99_CHECK_EQ(c99_inl_add(10, 32), 42);
    C99_CHECK_EQ(c99_inl_mul(6, 7), 42);
    /* Taking function pointer requires the single external definition from TU 2 */
    int (*fp)(int, int) = c99_inl_add;
    C99_CHECK_EQ(fp(100, 23), 123);
    C99_CHECK_EQ(call_from_tu2(4, 5), 29);
    C99_END();
}
