/*
 * C17 Conformance Test — Class L (Multi-TU Clang ABI Interop)
 * Bidirectional slimcc-pir <-> clang pass & return of odd-sized structs
 * (5B, 7B, 9B, 13B, 15B, 17B, 23B, 33B), bitfield structs, _Alignas(32)
 * structs, and stack-spilled odd structs past 8 GP register args.
 */
#include "c17_test.h"
#include "interop.h"

#define IMPL_SLIMCC_ROT(N)                                          \
    struct Odd##N slimcc_rot##N(struct Odd##N s, uint8_t k) {       \
        struct Odd##N out;                                          \
        for (int i = 0; i < N; i++)                                 \
            out.b[i] = (uint8_t)(s.b[(i + 1) % N] ^ k);             \
        return out;                                                 \
    }

IMPL_SLIMCC_ROT(5)
IMPL_SLIMCC_ROT(7)
IMPL_SLIMCC_ROT(9)
IMPL_SLIMCC_ROT(13)
IMPL_SLIMCC_ROT(15)
IMPL_SLIMCC_ROT(17)
IMPL_SLIMCC_ROT(23)
IMPL_SLIMCC_ROT(33)

struct BfAbi slimcc_bf_step(struct BfAbi in) {
    struct BfAbi out = in;
    out.a = (out.a + 3u) & 31u;
    out.b = -out.b;
    out.c = (out.c + 100u) & 8191u;
    out.d = out.d - 5;
    return out;
}

struct Align32Abi slimcc_align32_step(struct Align32Abi in) {
    struct Align32Abi out;
    out.x = in.x * 3 + 7;
    out.y = in.y - 11;
    return out;
}

int slimcc_spilled_odd(long g0, long g1, long g2, long g3,
                       long g4, long g5, long g6, long g7,
                       struct Odd5 a, struct Odd9 b, struct Odd17 c) {
    int sum = (int)(g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7);
    for (int i = 0; i < 5; i++)  sum += a.b[i];
    for (int i = 0; i < 9; i++)  sum += b.b[i];
    for (int i = 0; i < 17; i++) sum += c.b[i];
    return sum;
}

#define CHECK_CLANG_ROT(N) do {                                     \
    struct Odd##N s;                                                \
    for (int i = 0; i < N; i++) s.b[i] = (uint8_t)(i * 7 + 3);      \
    struct Odd##N r = clang_rot##N(s, 0x3cu);                       \
    for (int i = 0; i < N; i++)                                     \
        C17_CHECK(r.b[i] == (uint8_t)((((i + 1) % N) * 7 + 3) ^ 0x3cu)); \
} while (0)

int main(void) {
    C17_BEGIN("l65_pos_bug_clang_odd_structs",
              "Bidirectional slimcc-pir <-> clang odd-sized 5B..33B, bitfield, align32 ABI");

    /* Direction 1: slimcc-pir caller -> clang callee */
    CHECK_CLANG_ROT(5);
    CHECK_CLANG_ROT(7);
    CHECK_CLANG_ROT(9);
    CHECK_CLANG_ROT(13);
    CHECK_CLANG_ROT(15);
    CHECK_CLANG_ROT(17);
    CHECK_CLANG_ROT(23);
    CHECK_CLANG_ROT(33);

    struct BfAbi bf = { .a = 10u, .b = -12, .c = 500u, .d = 20 };
    struct BfAbi bfr = clang_bf_step(bf);
    C17_CHECK(bfr.a == 13u && bfr.b == 12 && bfr.c == 600u && bfr.d == 15);

    struct Align32Abi al = { .x = 100, .y = 50 };
    struct Align32Abi alr = clang_align32_step(al);
    C17_CHECK(alr.x == 307 && alr.y == 39);

    struct Odd5 a5 = { { 1, 2, 3, 4, 5 } };
    struct Odd9 b9 = { { 1, 1, 1, 1, 1, 1, 1, 1, 1 } };
    struct Odd17 c17 = { { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 } };
    int sp = clang_spilled_odd(1, 2, 3, 4, 5, 6, 7, 8, a5, b9, c17);
    C17_CHECK(sp == 36 + 15 + 9 + 34);

    /* Direction 2: clang caller -> slimcc-pir callee */
    C17_CHECK(clang_verify_slimcc_odd_structs() == 0);

    C17_END();
}
