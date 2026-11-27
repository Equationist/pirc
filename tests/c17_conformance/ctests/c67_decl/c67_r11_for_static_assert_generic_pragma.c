/* Round 11 C17 _Static_assert in for-init, _Generic default, and _Pragma regressions */
#include "c17_test.h"

int g_x = _Generic(1, default: sizeof(struct R11GDef { int a; }));
int g_y = _Generic(1, default: sizeof(enum { R11_GDEF_E = 1 }));

#line _Pragma("unknown") 200
static int line_after_pragma(void) { return __LINE__; }

static int test_pragma_pack_positions(int a, _Pragma("pack(push, 1)") int b) {
    struct R11PackInFn { char c; int v; };
    _Pragma("pack(pop)")
    if (a) _Pragma("pack(push, 1)") ;
    _Pragma("pack(pop)")
    return a + b + (int)sizeof(struct R11PackInFn);
}

int main(void) {
    for (_Static_assert(1, "ok"); 0; ) {}
    int b_z = _Generic(1, default: sizeof(struct R11BDef { int a; int b; }));
    C17_CHECK(g_x == (int)sizeof(int));
    C17_CHECK(g_y == (int)sizeof(int));
    C17_CHECK(b_z == (int)(2 * sizeof(int)));
    C17_CHECK(line_after_pragma() == 200);
    C17_CHECK(test_pragma_pack_positions(10, 20) == 30 + (int)(sizeof(char) + sizeof(int)));
    C17_END();
}
