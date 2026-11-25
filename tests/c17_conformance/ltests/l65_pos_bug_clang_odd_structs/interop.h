/*
 * Shared header for bidirectional slimcc-pir <-> clang AAPCS64 ABI interop:
 * odd-sized structs (5B..33B), bitfield structs, _Alignas(32) structs, and
 * stack-spilled odd structs past 8 GP register arguments.
 */
#ifndef L65_ODD_STRUCTS_INTEROP_H
#define L65_ODD_STRUCTS_INTEROP_H

#include <stdalign.h>
#include <stdint.h>

struct Odd5  { uint8_t b[5]; };
struct Odd7  { uint8_t b[7]; };
struct Odd9  { uint8_t b[9]; };
struct Odd13 { uint8_t b[13]; };
struct Odd15 { uint8_t b[15]; };
struct Odd17 { uint8_t b[17]; };
struct Odd23 { uint8_t b[23]; };
struct Odd33 { uint8_t b[33]; };

struct BfAbi {
    unsigned a : 5;
    int      b : 6;
    unsigned c : 13;
    int      d : 8;
};

struct Align32Abi {
    _Alignas(32) int64_t x;
    int32_t y;
};

/* Defined in Clang TU, called from slimcc-pir TU */
struct Odd5  clang_rot5(struct Odd5 s, uint8_t k);
struct Odd7  clang_rot7(struct Odd7 s, uint8_t k);
struct Odd9  clang_rot9(struct Odd9 s, uint8_t k);
struct Odd13 clang_rot13(struct Odd13 s, uint8_t k);
struct Odd15 clang_rot15(struct Odd15 s, uint8_t k);
struct Odd17 clang_rot17(struct Odd17 s, uint8_t k);
struct Odd23 clang_rot23(struct Odd23 s, uint8_t k);
struct Odd33 clang_rot33(struct Odd33 s, uint8_t k);
struct BfAbi clang_bf_step(struct BfAbi in);
struct Align32Abi clang_align32_step(struct Align32Abi in);
int clang_spilled_odd(long g0, long g1, long g2, long g3,
                      long g4, long g5, long g6, long g7,
                      struct Odd5 a, struct Odd9 b, struct Odd17 c);
int clang_verify_slimcc_odd_structs(void);

/* Defined in slimcc-pir TU, called from Clang TU */
struct Odd5  slimcc_rot5(struct Odd5 s, uint8_t k);
struct Odd7  slimcc_rot7(struct Odd7 s, uint8_t k);
struct Odd9  slimcc_rot9(struct Odd9 s, uint8_t k);
struct Odd13 slimcc_rot13(struct Odd13 s, uint8_t k);
struct Odd15 slimcc_rot15(struct Odd15 s, uint8_t k);
struct Odd17 slimcc_rot17(struct Odd17 s, uint8_t k);
struct Odd23 slimcc_rot23(struct Odd23 s, uint8_t k);
struct Odd33 slimcc_rot33(struct Odd33 s, uint8_t k);
struct BfAbi slimcc_bf_step(struct BfAbi in);
struct Align32Abi slimcc_align32_step(struct Align32Abi in);
int slimcc_spilled_odd(long g0, long g1, long g2, long g3,
                       long g4, long g5, long g6, long g7,
                       struct Odd5 a, struct Odd9 b, struct Odd17 c);

#endif
