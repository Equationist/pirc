/*
 * C99 §6.5.5, §6.5.7, §6.5.10-§6.5.12: Truncated-toward-zero signed division
 * and remainder (new mandatory rule in C99!), 64-bit div/rem, shifts, bitwise ops.
 */
#include "c17_test.h"
#include <stdint.h>

int main(void) {
    C99_BEGIN("c65_shifts_div_bitops", "C99 truncating div/rem, 64-bit div/rem, shifts, bitwise");

    /* C99 §6.5.5 p6: (a/b)*b + a%b == a, and a/b truncates toward zero! */
    int p7 = c99_opaque_int(7);
    int n7 = c99_opaque_int(-7);
    int p2 = c99_opaque_int(2);
    int n2 = c99_opaque_int(-2);

    C99_CHECK_EQ(p7 / p2, 3);
    C99_CHECK_EQ(p7 % p2, 1);
    C99_CHECK_EQ(n7 / p2, -3);
    C99_CHECK_EQ(n7 % p2, -1);
    C99_CHECK_EQ(p7 / n2, -3);
    C99_CHECK_EQ(p7 % n2, 1);
    C99_CHECK_EQ(n7 / n2, 3);
    C99_CHECK_EQ(n7 % n2, -1);

    /* 64-bit signed and unsigned division & remainder */
    long long s64 = c99_opaque_ll(-1000000000007LL);
    long long d64 = c99_opaque_ll(10LL);
    C99_CHECK_EQ(s64 / d64, -100000000000LL);
    C99_CHECK_EQ(s64 % d64, -7LL);

    unsigned long long u64 = c99_opaque_ull(0xFFFFFFFFFFFFFFFFULL);
    unsigned long long ud = c99_opaque_ull(10ULL);
    C99_CHECK_UEQ(u64 / ud, 1844674407370955161ULL);
    C99_CHECK_UEQ(u64 % ud, 5ULL);

    /* Unsigned shifts including bit 31 / bit 63 and shift by 0 */
    int sh0 = c99_opaque_int(0);
    int sh31 = c99_opaque_int(31);
    int sh63 = c99_opaque_int(63);
    unsigned int u1 = (unsigned int)c99_opaque_int(1);
    unsigned long long ull1 = c99_opaque_ull(1ULL);

    C99_CHECK_UEQ(u1 << sh0, 1u);
    C99_CHECK_UEQ(u1 << sh31, 0x80000000u);
    C99_CHECK_UEQ((u1 << sh31) >> sh31, 1u);
    C99_CHECK_UEQ(ull1 << sh0, 1ULL);
    C99_CHECK_UEQ(ull1 << sh63, 0x8000000000000000ULL);
    C99_CHECK_UEQ((ull1 << sh63) >> sh63, 1ULL);

    /* Bitwise ops across widths */
    uint8_t b8 = (uint8_t)c99_opaque_int(0x5Au);
    uint16_t b16 = (uint16_t)c99_opaque_int(0xA55Au);
    uint64_t b64 = c99_opaque_ull(0x0123456789ABCDEFULL);
    C99_CHECK_EQ((unsigned char)(~b8), 0xA5u);
    C99_CHECK_EQ((unsigned short)(~b16), 0x5AA5u);
    C99_CHECK_UEQ(~b64, 0xFEDCBA9876543210ULL);
    C99_CHECK_UEQ((b64 ^ 0xFF00FF00FF00FF00ULL) ^ 0xFF00FF00FF00FF00ULL, b64);

    C99_END();
}
