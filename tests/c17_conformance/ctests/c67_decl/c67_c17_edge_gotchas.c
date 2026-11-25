/*
 * C17-specific DR & spec edge gotchas:
 *  - DR 481 (§6.5.1.1): _Generic lvalue conversion strips _Atomic!
 *  - _Generic string literal decays to 'char *' (NOT 'const char *'!)
 *  - §6.7.5 p6: _Alignas(0) has no effect; multiple _Alignas take strictest
 *  - Anonymous struct/union inside array of structs & compound literal
 */
#include "c17_test.h"
#include <stdatomic.h>
#include <stdint.h>

struct AnonItem {
    int id;
    union {
        struct { int x, y; };
        long long packed;
    };
};

int main(void) {
    C17_BEGIN("c67_c17_edge_gotchas", "C17 DR 481 _Generic(_Atomic), char* strlit, _Alignas(0), anon arrays");

    /* C17 DR 481: _Generic controlling expr strips _Atomic */
    _Atomic int ai = 5;
    C17_CHECK_EQ(_Generic(ai, int: 1, default: 0), 1);

    /* String literal has type char[N] in C, decaying to char * in _Generic! */
    C17_CHECK_EQ(_Generic("abc", char *: 1, const char *: 2, default: 0), 1);

    /* _Alignas(0) has no effect; multiple _Alignas use the strictest */
    _Alignas(0) int a0 = 11;
    _Alignas(4) _Alignas(32) _Alignas(8) int a32 = 22;
    C17_CHECK_EQ(a0 + a32, 33);
    C17_CHECK_EQ((uintptr_t)&a32 % 32, 0);

    /* Array of structs with anonymous members + compound literal */
    struct AnonItem items[2] = {
        { .id = 1, .x = 10, .y = 20 },
        (struct AnonItem){ .id = 2, .x = 30, .y = 40 },
    };
    C17_CHECK_EQ(items[0].x + items[0].y, 30);
    C17_CHECK_EQ(items[1].x + items[1].y, 70);

    C17_END();
}
