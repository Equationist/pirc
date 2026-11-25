/* Cross-TU struct/HFA/bitfield/varargs ABI test (TU 2 - callee) */
#include "abi_common.h"
#include <stdarg.h>

struct Odd3 xtu_odd3(struct Odd3 s) {
    struct Odd3 r = { (char)(s.a * 2), (char)(s.b * 2), (char)(s.c * 2) };
    return r;
}

struct Big24 xtu_big24(struct Big24 a, struct Big24 b) {
    struct Big24 r = { a.x + b.x, a.y + b.y, a.z + b.z };
    return r;
}

struct Hfa4 xtu_hfa4(struct Hfa4 a, struct Hfa4 b) {
    struct Hfa4 r = { a.a + b.a, a.b + b.b, a.c + b.c, a.d + b.d };
    return r;
}

struct BfPair xtu_bf(struct BfPair p) {
    p.lo += 1u;
    p.hi += 1;
    return p;
}

double xtu_varargs(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double s = 0.0;
    for (int i = 0; i < n; i++) {
        s += va_arg(ap, double);
    }
    va_end(ap);
    return s;
}
