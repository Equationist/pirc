/* Round 12 C17 expr/types: _Generic default lexical scope, nested VLA ternary, DR 341, void* deref, ptr-diff static init */
#include "c17_test.h"

enum { GX = 1 };
struct GS { char c; };

static int test_generic_fwd_shadow(void) {
    return _Generic(0, default: (enum { GX = 2 })0, int: GX);
}

static int test_generic_rev_shadow(void) {
    return (int)_Generic(1.0,
        default: sizeof(struct GS),
        int: sizeof(struct GS { int a[10]; })
    );
}

static int test_generic_def_tag_visible(void) {
    return _Generic(0, default: (struct GDef { int x; }){0}.x, struct GDef: 1, int: 9);
}

static int test_generic_void_deref(void *vp) {
    return _Generic(*vp, default: 77);
}

static int test_generic_fnptr_star(void) {
    return _Generic(0, void (*)(int (*)[*]): 1, default: 2);
}

static int test_goto_over_fnptr_typedef(void) {
    goto L;
    typedef void (*FP)(int (*)[*]);
L:;
    return (FP)0 == 0 ? 1 : 0;
}

static int test_nested_vla_ternary(int c0, int c1, int c2, int n) {
    int (*p1)[n] = 0, (*p2)[n] = 0, (*p3)[n] = 0, (*p4)[n] = 0;
    int (*pu)[] = 0;
    int s1 = (int)sizeof(*(c0 ? (c1 ? p1 : p2) : (c2 ? p3 : p4)));
    int s2 = (int)sizeof(*(c1 ? (c2 ? p1 : p2) : pu));
    return s1 + s2;
}

static int g_a[4], g_b[4];
static long g_d1 = -(&g_b[3] - &g_b[1]);
static long g_d2 = (&g_b[3] - &g_b[1]) * 3;
static int *g_p = &g_a[0] + (&g_b[2] - &g_b[0]);

int main(void) {
    C17_CHECK(test_generic_fwd_shadow() == 2);
    C17_CHECK(test_generic_rev_shadow() == 1);
    C17_CHECK(test_generic_def_tag_visible() == 9);
    int dummy = 0;
    C17_CHECK(test_generic_void_deref(&dummy) == 77);
    C17_CHECK(test_generic_fnptr_star() == 2);
    C17_CHECK(test_goto_over_fnptr_typedef() == 1);
    C17_CHECK(test_nested_vla_ternary(1, 0, 1, 5) == 2 * 5 * (int)sizeof(int));
    C17_CHECK(g_d1 == -2L);
    C17_CHECK(g_d2 == 6L);
    C17_CHECK(g_p == &g_a[2]);
    C17_END();
}
