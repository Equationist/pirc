/* Round 12 codegen: volatile/_Atomic aggregate assign alignment, atomic ptr ops, packed ptr casts */
#include "c17_test.h"
#include <stdatomic.h>

#pragma pack(push, 1)
struct P8 { int a; int b; };
struct WrapP8 { char c; struct P8 p[2]; };
#pragma pack(pop)

struct S8 { int a; int b; };
#pragma pack(push, 1)
struct PS8 { char c; struct S8 s; };
#pragma pack(pop)

static int read_p8_0(struct P8 *p) { return *(int *)p; }
static int read_p8_1(struct P8 *p) { return *(int *)(p + 1); }

int main(void) {
    struct PS8 ps = { 1, { 10, 20 } };
    volatile struct S8 v;
    _Atomic struct S8 a;
    C17_CHECK((v = ps.s).a == 10);
    C17_CHECK((a = ps.s).b == 20);
    volatile struct S8 v2;
    v2 = (v = ps.s);
    C17_CHECK(v2.a == 10 && v2.b == 20);

    _Alignas(8) unsigned char buf[16] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    unsigned char * _Atomic cp;
    atomic_init(&cp, buf + 1);
    C17_CHECK(*(int *)(cp++) == 0x04030201);
    C17_CHECK(*(int *)atomic_exchange(&cp, buf + 1) == 0x05040302);
    unsigned char *p = buf;
    C17_CHECK(*(int *)(++p) == 0x04030201);

    struct WrapP8 w = { 1, { { 11, 22 }, { 33, 44 } } };
    C17_CHECK(read_p8_0(&w.p[0]) == 11);
    C17_CHECK(read_p8_1(w.p) == 33);
    C17_END();
}
