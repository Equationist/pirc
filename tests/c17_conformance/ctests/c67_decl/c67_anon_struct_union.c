/*
 * C17 §6.7.2.1 p13: Anonymous structures and unions — multi-level nesting,
 * designated initializers targeting anonymous members directly, offsetof,
 * and pass/return by value.
 */
#include "c17_test.h"
#include <stddef.h>

struct Packet {
    int kind;
    union {
        struct {
            int x;
            int y;
        };
        struct {
            double re;
            double im;
        };
        long long raw;
    };
    struct {
        short flags;
        struct {
            unsigned char lo;
            unsigned char hi;
        };
    };
};

static struct Packet shift_packet(struct Packet p, int dx, int dy) {
    p.x += dx;
    p.y += dy;
    p.lo = (unsigned char)(p.lo + 1);
    return p;
}

int main(void) {
    C17_BEGIN("c67_anon_struct_union", "C17 anonymous structs/unions, designated init, nesting");

    /* Designated initializer directly naming anonymous members! */
    struct Packet p1 = {
        .kind = 1,
        .x = 10,
        .y = 20,
        .flags = 7,
        .lo = 0x10,
        .hi = 0x20,
    };
    C17_CHECK_EQ(p1.kind, 1);
    C17_CHECK_EQ(p1.x, 10);
    C17_CHECK_EQ(p1.y, 20);
    C17_CHECK_EQ(p1.flags, 7);
    C17_CHECK_EQ(p1.lo, 0x10);
    C17_CHECK_EQ(p1.hi, 0x20);

    /* Pass and return by value */
    struct Packet p2 = shift_packet(p1, 5, -3);
    C17_CHECK_EQ(p2.x, 15);
    C17_CHECK_EQ(p2.y, 17);
    C17_CHECK_EQ(p2.lo, 0x11);
    C17_CHECK_EQ(p2.hi, 0x20);

    /* Alternate anonymous union branch */
    struct Packet p3 = { .kind = 2, .re = 1.5, .im = -2.5 };
    C17_CHECK_EQ(p3.kind, 2);
    C17_CHECK_DBL(p3.re, 1.5, 0.0);
    C17_CHECK_DBL(p3.im, -2.5, 0.0);

    /* offsetof on deeply nested anonymous members */
    C17_CHECK_EQ((char *)&p1.x - (char *)&p1, (ptrdiff_t)offsetof(struct Packet, x));
    C17_CHECK_EQ((char *)&p1.hi - (char *)&p1, (ptrdiff_t)offsetof(struct Packet, hi));

    C17_END();
}
