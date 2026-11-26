/* C17 §6.7.5p6, §6.7.9p9, §6.10.4:
 * - _Alignas(0) has no effect and is valid
 * - Unnamed bitfields are skipped during struct initialization
 * - Many #line filename switches (debug file table dynamic growth) */
#include "c17_test.h"

struct WithUnnamedBF {
    int : 4;
    int a;
    int : 0;
    int b;
};

struct NestedBF {
    struct WithUnnamedBF inner;
    int tail;
};

static struct WithUnnamedBF g_s = { 42, 43 };
static struct NestedBF g_n = { 77, 78, 88 };
_Alignas(0) int g_al0 = 123;

#line 10 "r6_virt_1.c"
static int vf1(void) { return 1; }
#line 20 "r6_virt_2.c"
static int vf2(void) { return 2; }
#line 30 "r6_virt_3.c"
static int vf3(void) { return 3; }
#line 40 "r6_virt_4.c"
static int vf4(void) { return 4; }
#line 50 "r6_virt_5.c"
static int vf5(void) { return 5; }
#line 60 "r6_virt_6.c"
static int vf6(void) { return 6; }
#line 70 "r6_virt_7.c"
static int vf7(void) { return 7; }
#line 80 "r6_virt_8.c"
static int vf8(void) { return 8; }
#line 90 "r6_virt_9.c"
static int vf9(void) { return 9; }
#line 100 "r6_virt_10.c"
static int vf10(void) { return 10; }

int main(void) {
    C17_CHECK(g_s.a == 42 && g_s.b == 43);
    C17_CHECK(g_n.inner.a == 77 && g_n.inner.b == 78 && g_n.tail == 88);
    C17_CHECK(g_al0 == 123);

    struct WithUnnamedBF ls = { 55, 56 };
    struct NestedBF ln = { 33, 34, 44 };
    _Alignas(0) int la0 = 99;
    C17_CHECK(ls.a == 55 && ls.b == 56);
    C17_CHECK(ln.inner.a == 33 && ln.inner.b == 34 && ln.tail == 44);
    C17_CHECK(la0 == 99);

    int sum = vf1() + vf2() + vf3() + vf4() + vf5() +
              vf6() + vf7() + vf8() + vf9() + vf10();
    C17_CHECK(sum == 55);

    C17_END();
}
