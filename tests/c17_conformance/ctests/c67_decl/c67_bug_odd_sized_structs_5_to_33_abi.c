/*
 * C17 Conformance Test — Class C (§6.5.2.2 / AAPCS64 & SysV ABI)
 * Exhaustive odd-sized struct pass-by-value, return-by-value, and aliased sret
 * stress for sizes 5, 6, 7, 9, 10, 11, 13, 14, 15, 17, 19, 23, 31, and 33 bytes.
 */
#include "c17_test.h"
#include <stdint.h>

#define DEF_ODD_STRUCT(N)                                           \
    struct S##N { uint8_t b[N]; };                                  \
    static struct S##N make_s##N(uint8_t base) {                    \
        struct S##N s;                                              \
        for (int i = 0; i < N; i++)                                 \
            s.b[i] = (uint8_t)((base + i * 13) & 0xff);             \
        return s;                                                   \
    }                                                               \
    static struct S##N rot_s##N(struct S##N in, uint8_t k) {        \
        struct S##N out;                                            \
        for (int i = 0; i < N; i++)                                 \
            out.b[i] = (uint8_t)(in.b[(i + 1) % N] ^ k);            \
        return out;                                                 \
    }                                                               \
    static int check_s##N(uint8_t base, uint8_t k) {                \
        struct S##N s = make_s##N(base);                            \
        s = rot_s##N(s, k); /* aliased sret: s = f(s) */            \
        for (int i = 0; i < N; i++) {                               \
            uint8_t exp = (uint8_t)(((base + ((i + 1) % N) * 13) & 0xff) ^ k); \
            if (s.b[i] != exp) return 0;                            \
        }                                                           \
        return (sizeof(struct S##N) == N) ? 1 : 0;                  \
    }

DEF_ODD_STRUCT(5)
DEF_ODD_STRUCT(6)
DEF_ODD_STRUCT(7)
DEF_ODD_STRUCT(9)
DEF_ODD_STRUCT(10)
DEF_ODD_STRUCT(11)
DEF_ODD_STRUCT(13)
DEF_ODD_STRUCT(14)
DEF_ODD_STRUCT(15)
DEF_ODD_STRUCT(17)
DEF_ODD_STRUCT(19)
DEF_ODD_STRUCT(23)
DEF_ODD_STRUCT(31)
DEF_ODD_STRUCT(33)

int main(void) {
    C17_BEGIN("c67_bug_odd_sized_structs_5_to_33_abi",
              "Odd-sized structs 5B..33B pass/return/aliased-sret ABI stress");

    uint8_t b = (uint8_t)c17_opaque_int(17);
    uint8_t k = (uint8_t)c17_opaque_int(0x5a);

    C17_CHECK(check_s5(b, k)  == 1);
    C17_CHECK(check_s6(b, k)  == 1);
    C17_CHECK(check_s7(b, k)  == 1);
    C17_CHECK(check_s9(b, k)  == 1);
    C17_CHECK(check_s10(b, k) == 1);
    C17_CHECK(check_s11(b, k) == 1);
    C17_CHECK(check_s13(b, k) == 1);
    C17_CHECK(check_s14(b, k) == 1);
    C17_CHECK(check_s15(b, k) == 1);
    C17_CHECK(check_s17(b, k) == 1);
    C17_CHECK(check_s19(b, k) == 1);
    C17_CHECK(check_s23(b, k) == 1);
    C17_CHECK(check_s31(b, k) == 1);
    C17_CHECK(check_s33(b, k) == 1);

    C17_END();
}
