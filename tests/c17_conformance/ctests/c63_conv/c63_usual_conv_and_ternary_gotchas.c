/*
 * C17 §6.3.1 & §6.5.15: Using _Generic to verify exact static result types of
 * ternary ?:, integer promotions, usual arithmetic conversions, qualified
 * pointer merging, and null pointer constants vs non-null void*.
 */
#include "c17_test.h"

struct BfPromo {
    unsigned int u5 : 5;
    unsigned int u32 : 32;
};

int main(void) {
    C17_BEGIN("c63_usual_conv_and_ternary_gotchas", "Exact static types of ?:, promotions, and ptr merges via _Generic");

    int cond = c17_opaque_int(1);
    int x = 1;
    const int *pc = &x;
    volatile int *pv = &x;
    void *vp = &x;

    /* Ternary numeric promotions: result type is common type regardless of branch */
    C17_CHECK_EQ(_Generic(cond ? 1 : 2.5, double: 1, default: 0), 1);
    C17_CHECK_EQ(_Generic(cond ? 1u : -1, unsigned int: 1, default: 0), 1);
    C17_CHECK_EQ(_Generic(cond ? 1u : -1LL, long long: 1, default: 0), 1);

    /* Ternary combines qualifiers from both pointer branches: const + volatile -> const volatile! */
    C17_CHECK_EQ(_Generic(cond ? pc : pv, const volatile int *: 1, default: 0), 1);

    /* Null pointer constant (void *)0 keeps the other pointer's type (int *);
     * non-constant void* converts result to void*! */
    C17_CHECK_EQ(_Generic(cond ? &x : (void *)0, int *: 1, void *: 2, default: 0), 1);
    C17_CHECK_EQ(_Generic(cond ? &x : vp, void *: 1, int *: 2, default: 0), 1);
    C17_CHECK_EQ(_Generic(cond ? pc : (void *)0, const int *: 1, default: 0), 1);
    C17_CHECK_EQ(_Generic(cond ? pc : vp, const void *: 1, default: 0), 1);

    /* Integer promotions verified statically */
    _Bool b = 1;
    unsigned char uc = 1;
    unsigned short us = 1;
    struct BfPromo bf = { 3u, 3u };
    C17_CHECK_EQ(_Generic(+b, int: 1, default: 0), 1);
    C17_CHECK_EQ(_Generic(+uc, int: 1, default: 0), 1);
    C17_CHECK_EQ(_Generic(+us, int: 1, default: 0), 1);
    C17_CHECK_EQ(_Generic(bf.u5 + 0, int: 1, unsigned int: 2, default: 0), 1);
    C17_CHECK_EQ(_Generic(bf.u32 + 0, unsigned int: 1, int: 2, default: 0), 1);

    /* Runtime check: false branch of (0 ? 1u : -1) converts -1 to UINT_MAX */
    unsigned int r = c17_opaque_int(0) ? 1u : -1;
    C17_CHECK_UEQ(r, ~0u);

    C17_END();
}
