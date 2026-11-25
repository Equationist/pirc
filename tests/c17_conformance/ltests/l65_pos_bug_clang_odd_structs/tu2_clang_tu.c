/*
 * Compiled by Clang: verifies true binary ABI compatibility with slimcc-pir
 * for odd-sized structs (5B..33B), bitfield structs, _Alignas(32) structs,
 * and stack-spilled odd structs.
 */
#include "interop.h"

#define IMPL_CLANG_ROT(N)                                           \
    struct Odd##N clang_rot##N(struct Odd##N s, uint8_t k) {        \
        struct Odd##N out;                                          \
        for (int i = 0; i < N; i++)                                 \
            out.b[i] = (uint8_t)(s.b[(i + 1) % N] ^ k);             \
        return out;                                                 \
    }

IMPL_CLANG_ROT(5)
IMPL_CLANG_ROT(7)
IMPL_CLANG_ROT(9)
IMPL_CLANG_ROT(13)
IMPL_CLANG_ROT(15)
IMPL_CLANG_ROT(17)
IMPL_CLANG_ROT(23)
IMPL_CLANG_ROT(33)

struct BfAbi clang_bf_step(struct BfAbi in) {
    struct BfAbi out = in;
    out.a = (out.a + 3u) & 31u;
    out.b = -out.b;
    out.c = (out.c + 100u) & 8191u;
    out.d = out.d - 5;
    return out;
}

struct Align32Abi clang_align32_step(struct Align32Abi in) {
    struct Align32Abi out;
    out.x = in.x * 3 + 7;
    out.y = in.y - 11;
    return out;
}

int clang_spilled_odd(long g0, long g1, long g2, long g3,
                      long g4, long g5, long g6, long g7,
                      struct Odd5 a, struct Odd9 b, struct Odd17 c) {
    int sum = (int)(g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7);
    for (int i = 0; i < 5; i++)  sum += a.b[i];
    for (int i = 0; i < 9; i++)  sum += b.b[i];
    for (int i = 0; i < 17; i++) sum += c.b[i];
    return sum;
}

#define VERIFY_SLIMCC_ROT(N, CODE) do {                             \
    struct Odd##N s;                                                \
    for (int i = 0; i < N; i++) s.b[i] = (uint8_t)(i * 7 + 3);      \
    struct Odd##N r = slimcc_rot##N(s, 0x3cu);                      \
    for (int i = 0; i < N; i++)                                     \
        if (r.b[i] != (uint8_t)((((i + 1) % N) * 7 + 3) ^ 0x3cu))   \
            return CODE;                                            \
} while (0)

int clang_verify_slimcc_odd_structs(void) {
    VERIFY_SLIMCC_ROT(5, 1);
    VERIFY_SLIMCC_ROT(7, 2);
    VERIFY_SLIMCC_ROT(9, 3);
    VERIFY_SLIMCC_ROT(13, 4);
    VERIFY_SLIMCC_ROT(15, 5);
    VERIFY_SLIMCC_ROT(17, 6);
    VERIFY_SLIMCC_ROT(23, 7);
    VERIFY_SLIMCC_ROT(33, 8);

    struct BfAbi bf = { .a = 10u, .b = -12, .c = 500u, .d = 20 };
    struct BfAbi bfr = slimcc_bf_step(bf);
    if (!(bfr.a == 13u && bfr.b == 12 && bfr.c == 600u && bfr.d == 15))
        return 9;

    struct Align32Abi al = { .x = 100, .y = 50 };
    struct Align32Abi alr = slimcc_align32_step(al);
    if (!(alr.x == 307 && alr.y == 39))
        return 10;

    struct Odd5 a5 = { { 1, 2, 3, 4, 5 } };
    struct Odd9 b9 = { { 1, 1, 1, 1, 1, 1, 1, 1, 1 } };
    struct Odd17 c17 = { { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 } };
    if (slimcc_spilled_odd(1, 2, 3, 4, 5, 6, 7, 8, a5, b9, c17) != 36 + 15 + 9 + 34)
        return 11;

    return 0;
}
