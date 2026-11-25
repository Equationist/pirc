/*
 * C17 Conformance Test — Class C (§6.7.9 / §6.5.2.5 / §6.7.2.1)
 * Adversarial regression for:
 *   1) Deeply nested designated initializers (array of struct of union of array)
 *      in both static and automatic storage duration, including designator overrides.
 *   2) File-scope pointer initialized to address of a compound literal (&(int){42}).
 *   3) Valid ISO C17 struct with flexible array member initialized WITHOUT
 *      initializing the FAM (struct Fam f = { 7 };).
 */
#include "c17_test.h"
#include <stdint.h>

struct Inner {
    int tag;
    union {
        int32_t pair[2];
        struct { int16_t lo; int16_t hi; } split;
    } u;
};

static const struct Inner g_nested[3] = {
    [2] = { .tag = 30, .u.pair = { [1] = 99, [0] = 88 } },
    [0] = { .tag = 10, .u.split = { .hi = 5, .lo = 3 } },
    [0].tag = 11, /* override earlier designator */
};

static int *g_cl_ptr = &(int){ 42 };
static int *g_cl_arr = (int[]){ 100, 200, 300 };

struct ValidFamInit {
    int count;
    int elems[];
};
static struct ValidFamInit g_fam_only_hdr = { 7 };

int main(void) {
    C17_BEGIN("c67_bug_nested_desig_init_and_static_cl",
              "Nested designated init + overrides, file-scope compound lit ptr, FAM hdr init");

    /* 1. Static nested designated init + override */
    C17_CHECK(g_nested[0].tag == 11);
    C17_CHECK(g_nested[0].u.split.lo == 3 && g_nested[0].u.split.hi == 5);
    C17_CHECK(g_nested[1].tag == 0 && g_nested[1].u.pair[0] == 0 && g_nested[1].u.pair[1] == 0);
    C17_CHECK(g_nested[2].tag == 30 && g_nested[2].u.pair[0] == 88 && g_nested[2].u.pair[1] == 99);

    /* Automatic nested designated init with runtime values + override */
    int v = c17_opaque_int(15);
    struct Inner local[3] = {
        [1] = { .tag = v, .u.pair = { [0] = v + 1, [1] = v + 2 } },
        [2].u.pair[1] = v * 2,
        [1].u.pair[0] = v + 100,
    };
    C17_CHECK(local[0].tag == 0 && local[0].u.pair[0] == 0);
    C17_CHECK(local[1].tag == 15 && local[1].u.pair[0] == 115 && local[1].u.pair[1] == 17);
    C17_CHECK(local[2].tag == 0 && local[2].u.pair[0] == 0 && local[2].u.pair[1] == 30);

    /* 2. File-scope compound literal pointers */
    C17_CHECK(*g_cl_ptr == 42);
    *g_cl_ptr = 84;
    C17_CHECK(*g_cl_ptr == 84);
    C17_CHECK(g_cl_arr[0] == 100 && g_cl_arr[1] == 200 && g_cl_arr[2] == 300);

    /* 3. Valid C17 FAM header-only initialization */
    C17_CHECK(g_fam_only_hdr.count == 7);
    struct ValidFamInit local_fam = { .count = 9 };
    C17_CHECK(local_fam.count == 9);

    C17_END();
}
