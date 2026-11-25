/*
 * C99 §6.5.16.2 p3: Compound assignment (E1 op= E2) MUST evaluate E1 only once,
 * including array subscripts, dereferenced function results, bitfields, and
 * mixed arithmetic types.
 */
#include "c17_test.h"

struct BfHolder {
    unsigned int low : 4;
    signed int mid : 6;
    unsigned int hi : 6;
};

static int idx_evals = 0;
static int ptr_evals = 0;
static int bf_evals = 0;

static int get_idx(int i) {
    idx_evals++;
    return i;
}

static int *get_elem_ptr(int *arr, int i) {
    ptr_evals++;
    return &arr[i];
}

static struct BfHolder *get_bf(struct BfHolder *p) {
    bf_evals++;
    return p;
}

int main(void) {
    C99_BEGIN("c65_compound_assign_eval_once", "Compound assignment evaluates LHS exactly once");

    int arr[4] = { 10, 20, 30, 40 };

    /* Array subscript LHS */
    arr[get_idx(1)] += 5;
    C99_CHECK_EQ(idx_evals, 1);
    C99_CHECK_EQ(arr[1], 25);

    arr[get_idx(2)] *= 3;
    C99_CHECK_EQ(idx_evals, 2);
    C99_CHECK_EQ(arr[2], 90);

    arr[get_idx(3)] >>= 2;
    C99_CHECK_EQ(idx_evals, 3);
    C99_CHECK_EQ(arr[3], 10);

    /* Dereferenced pointer-returning function LHS */
    (*get_elem_ptr(arr, 0)) ^= 0x0f;
    C99_CHECK_EQ(ptr_evals, 1);
    C99_CHECK_EQ(arr[0], 10 ^ 0x0f);

    (*get_elem_ptr(arr, 1)) %= 7;
    C99_CHECK_EQ(ptr_evals, 2);
    C99_CHECK_EQ(arr[1], 4);

    /* Bitfield member via function call LHS */
    struct BfHolder bh = { 3u, -5, 10u };
    get_bf(&bh)->low += 9u;
    C99_CHECK_EQ(bf_evals, 1);
    C99_CHECK_EQ(bh.low, 12u);

    get_bf(&bh)->mid -= 4;
    C99_CHECK_EQ(bf_evals, 2);
    C99_CHECK_EQ(bh.mid, -9);

    /* Post/pre increment on side-effecting LHS */
    int old = arr[get_idx(0)]++;
    C99_CHECK_EQ(idx_evals, 4);
    C99_CHECK_EQ(old, 5);
    C99_CHECK_EQ(arr[0], 6);

    /* Mixed-type compound assignment: conversion occurs BEFORE narrowing store */
    int x = 10;
    x *= 1.5; /* (double)10 * 1.5 = 15.0 -> 15 (NOT 10 * (int)1.5 = 10!) */
    C99_CHECK_EQ(x, 15);

    unsigned char uc = 200;
    uc += 100; /* 300 mod 256 = 44 */
    C99_CHECK_EQ(uc, 44);

    /* Value of compound assignment expression is the stored (converted) value */
    unsigned char uc2 = 250;
    int res = (uc2 += 10);
    C99_CHECK_EQ(uc2, 4);
    C99_CHECK_EQ(res, 4);

    C99_END();
}
