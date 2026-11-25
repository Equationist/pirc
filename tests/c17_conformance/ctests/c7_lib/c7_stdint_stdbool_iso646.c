/*
 * C99 §7.8 (<inttypes.h>), §7.9 (<iso646.h>), §7.16 (<stdbool.h>),
 * §7.17 (<stddef.h>), §7.18 (<stdint.h>), §5.2.4.2 (<limits.h>, <float.h>).
 */
#include "c17_test.h"
#include <float.h>
#include <inttypes.h>
#include <iso646.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct NestedOff {
    char a;
    struct {
        short b;
        int arr[4];
    } sub;
    double d;
};

int main(void) {
    C99_BEGIN("c7_stdint_stdbool_iso646", "stdint, inttypes, stdbool, iso646, offsetof, limits, float");

    /* <stdbool.h> */
    C99_CHECK_EQ(__bool_true_false_are_defined, 1);
    bool t = true;
    bool f = false;
    C99_CHECK_EQ(t, 1);
    C99_CHECK_EQ(f, 0);

    /* <stdint.h> exact widths & constant macros */
    C99_CHECK_EQ(sizeof(int8_t), 1);
    C99_CHECK_EQ(sizeof(uint8_t), 1);
    C99_CHECK_EQ(sizeof(int16_t), 2);
    C99_CHECK_EQ(sizeof(uint16_t), 2);
    C99_CHECK_EQ(sizeof(int32_t), 4);
    C99_CHECK_EQ(sizeof(uint32_t), 4);
    C99_CHECK_EQ(sizeof(int64_t), 8);
    C99_CHECK_EQ(sizeof(uint64_t), 8);
    C99_CHECK_EQ(INT64_C(-1234567890123), -1234567890123LL);
    C99_CHECK_UEQ(UINT64_C(0xFEDCBA9876543210), 0xFEDCBA9876543210ULL);

    /* intptr_t / uintptr_t round-trip */
    int target = 42;
    uintptr_t up = (uintptr_t)&target;
    int *back = (int *)up;
    C99_CHECK(back == &target);
    C99_CHECK_EQ(*back, 42);

    /* <inttypes.h> format macros */
    char buf[64];
    int64_t s64 = INT64_C(-9000000000000);
    uint64_t u64 = UINT64_C(0x123456789abcdef0);
    snprintf(buf, sizeof(buf), "%" PRId64 ":%" PRIx64, s64, u64);
    C99_CHECK_STR(buf, "-9000000000000:123456789abcdef0");

    /* <iso646.h> alternative operator spellings */
    int x = 0x0F;
    int y = 0x33;
    C99_CHECK((x bitand y) == 0x03);
    C99_CHECK((x bitor y) == 0x3F);
    C99_CHECK((x xor y) == 0x3C);
    C99_CHECK((x not_eq y) and not (x == y));
    C99_CHECK((x == 0) or (y == 0x33));
    x xor_eq 0x0F;
    C99_CHECK_EQ(x, 0);

    /* <stddef.h> offsetof on nested structs & array elements */
    struct NestedOff no;
    C99_CHECK_EQ(offsetof(struct NestedOff, a), 0);
    C99_CHECK_EQ((char *)&no.sub.arr[2] - (char *)&no, (ptrdiff_t)offsetof(struct NestedOff, sub.arr[2]));
    C99_CHECK_EQ((char *)&no.d - (char *)&no, (ptrdiff_t)offsetof(struct NestedOff, d));

    /* <limits.h> & <float.h> sanity */
    C99_CHECK_EQ(CHAR_BIT, 8);
    C99_CHECK(LLONG_MAX >= 9223372036854775807LL);
    C99_CHECK(FLT_EPSILON > 0.0f && FLT_EPSILON < 1e-6f);
    C99_CHECK(DBL_EPSILON > 0.0 && DBL_EPSILON < 1e-15);

    C99_END();
}
