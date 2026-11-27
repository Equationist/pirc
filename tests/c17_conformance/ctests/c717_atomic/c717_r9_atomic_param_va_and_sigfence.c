/* C17 §7.17 / §6.5.2.2: _Atomic aggregate param staging, variadic/K&R passing, and signal_fence */
#include "c17_test.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>

struct S3 { uint8_t b[3]; };
struct S5 { uint8_t b[5]; };
struct Hfa2 { double a, b; };

static int take_atomic_s3(_Atomic struct S3 s) {
    struct S3 v = atomic_load(&s);
    return (int)v.b[0] + (int)v.b[1] + (int)v.b[2];
}

static int take_atomic_s5(_Atomic struct S5 s) {
    struct S5 v = atomic_load(&s);
    return (int)v.b[0] + (int)v.b[4];
}

static double va_hfa(int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    struct Hfa2 h = va_arg(ap, struct Hfa2);
    va_end(ap);
    return tag + h.a + h.b;
}

static int va_s3(int base, ...) {
    va_list ap;
    va_start(ap, base);
    struct S3 s = va_arg(ap, struct S3);
    int tail = va_arg(ap, int);
    va_end(ap);
    return base + s.b[0] + s.b[1] + s.b[2] + tail;
}

static double kr_hfa(h)
struct Hfa2 h;
{
    return h.a * 2.0 + h.b * 3.0;
}

static int g_sf = 0;

int main(void) {
    struct { struct S3 s; uint8_t guard[5]; } g3 = { {{ 10, 20, 30 }}, { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE } };
    C17_CHECK(take_atomic_s3(g3.s) == 60);
    C17_CHECK(g3.guard[0] == 0xAA && g3.guard[4] == 0xEE);

    struct { struct S5 s; uint8_t guard[3]; } g5 = { {{ 7, 1, 2, 3, 35 }}, { 0x11, 0x22, 0x33 } };
    C17_CHECK(take_atomic_s5(g5.s) == 42);
    C17_CHECK(g5.guard[0] == 0x11 && g5.guard[2] == 0x33);

    _Atomic struct Hfa2 ah = (struct Hfa2){ 1.5, 2.5 };
    _Atomic struct S3 as3 = (struct S3){ { 1, 2, 3 } };
    double (*fp_hfa)() = kr_hfa;

    C17_CHECK(va_hfa(10, ah) == 14.0);
    C17_CHECK(fp_hfa(ah) == 10.5);
    C17_CHECK(va_s3(100, as3, 7) == 113);

    g_sf = 111;
    atomic_signal_fence(memory_order_seq_cst);
    g_sf = 222;
    C17_CHECK(g_sf == 222);

    C17_END();
}
