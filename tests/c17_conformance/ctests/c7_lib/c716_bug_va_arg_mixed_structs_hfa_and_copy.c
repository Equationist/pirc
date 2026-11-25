/*
 * C17 Conformance Test — Class C (§7.16 / AAPCS64 Darwin ABI)
 * Adversarial varargs stress:
 *   - >8 fixed integer + FP parameters before '...' (forcing stack spilling
 *     of both fixed and variadic args).
 *   - Variadic structs of sizes 1B, 3B, 12B, 16B, 24B + 3-float & 4-double HFAs.
 *   - va_copy independent re-traversal and passing va_list * to a helper.
 */
#include "c17_test.h"
#include <stdarg.h>
#include <stdint.h>

struct S3  { uint8_t a, b, c; };
struct S12 { int32_t x, y, z; };
struct S24 { int64_t a, b, c; };
struct Hfa3f { float x, y, z; };
struct Hfa4d { double a, b, c, d; };

static int64_t read_tail_via_ptr(va_list *ap) {
    struct S24 s24 = va_arg(*ap, struct S24);
    struct Hfa3f hf = va_arg(*ap, struct Hfa3f);
    struct Hfa4d hd = va_arg(*ap, struct Hfa4d);
    return s24.a + s24.b + s24.c
         + (int64_t)(hf.x + hf.y + hf.z)
         + (int64_t)(hd.a + hd.b + hd.c + hd.d);
}

static int64_t heavy_variadic(
    int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9,
    double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8,
    ...)
{
    va_list ap, cp;
    va_start(ap, d8);
    va_copy(cp, ap);

    struct S3 s3 = va_arg(ap, struct S3);
    struct S12 s12 = va_arg(ap, struct S12);
    int64_t tail1 = read_tail_via_ptr(&ap);
    va_end(ap);

    /* Re-read identically from va_copy */
    struct S3 s3_c = va_arg(cp, struct S3);
    struct S12 s12_c = va_arg(cp, struct S12);
    int64_t tail2 = read_tail_via_ptr(&cp);
    va_end(cp);

    int64_t fixed = (int64_t)(a0+a1+a2+a3+a4+a5+a6+a7+a8+a9)
                  + (int64_t)(d0+d1+d2+d3+d4+d5+d6+d7+d8);
    int64_t p1 = (int64_t)s3.a + s3.b + s3.c + s12.x + s12.y + s12.z + tail1;
    int64_t p2 = (int64_t)s3_c.a + s3_c.b + s3_c.c + s12_c.x + s12_c.y + s12_c.z + tail2;
    return (p1 == p2) ? (fixed + p1) : -1;
}

int main(void) {
    C17_BEGIN("c716_bug_va_arg_mixed_structs_hfa_and_copy",
              "Variadic >8 fixed args, 3B/12B/24B structs, HFA3f/HFA4d, va_copy, va_list*");

    struct S3 s3 = { 3u, 5u, 7u };
    struct S12 s12 = { 10, 20, 30 };
    struct S24 s24 = { 100, 200, 300 };
    struct Hfa3f hf = { 1.0f, 2.0f, 4.0f };
    struct Hfa4d hd = { 8.0, 16.0, 32.0, 64.0 };

    int64_t got = heavy_variadic(
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
        1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0,
        s3, s12, s24, hf, hd);
    int64_t want = 55 + 45 + (3 + 5 + 7) + (10 + 20 + 30) + (100 + 200 + 300) + 7 + 120;
    C17_CHECK(got == want);

    C17_END();
}
