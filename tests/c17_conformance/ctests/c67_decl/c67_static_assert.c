/*
 * C17 §6.7.10 & §7.2: _Static_assert at file scope, block scope, inside
 * struct and union declarations, and via <assert.h> static_assert.
 */
#include "c17_test.h"
#include <assert.h>
#include <stdint.h>

_Static_assert(sizeof(int) >= 4, "int must be at least 32 bits");
static_assert(sizeof(int64_t) == 8, "int64_t must be 8 bytes");

struct WithStaticAssert {
    int a;
    _Static_assert(sizeof(short) == 2, "short inside struct");
    double b;
    static_assert(1 + 1 == 2, "static_assert inside struct");
};

union UnionWithStaticAssert {
    int i;
    _Static_assert(sizeof(char) == 1, "char inside union");
    float f;
};

int main(void) {
    C17_BEGIN("c67_static_assert", "C17 _Static_assert at file, block, struct, and union scopes");

    _Static_assert(sizeof(struct WithStaticAssert) >= sizeof(int) + sizeof(double),
                   "struct size sanity");
    static_assert(sizeof(union UnionWithStaticAssert) >= sizeof(int),
                  "union size sanity");

    struct WithStaticAssert s = { 10, 2.5 };
    union UnionWithStaticAssert u;
    u.i = 42;
    C17_CHECK_EQ(s.a, 10);
    C17_CHECK_DBL(s.b, 2.5, 0.0);
    C17_CHECK_EQ(u.i, 42);

    C17_END();
}
