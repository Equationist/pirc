/*
 * C99 §6.7.2.1 & §6.9.1: Struct & union value semantics across odd/large/HFA
 * sizes, indirect calls returning structs, and K&R old-style definitions with
 * default argument promotions.
 */
#include "c17_test.h"

struct S1  { char a; };
struct S3  { char a, b, c; };
struct S7  { char b[7]; };
struct S9  { long long x; char y; };
struct S16 { long long a, b; };
struct S24 { long long a, b, c; };
struct S64 { long long v[8]; };
struct HFA3F { float x, y, z; };
struct HFA4D { double a, b, c, d; };
struct NonHFA5D { double v[5]; };

static struct S3 make_s3(char a, char b, char c) {
    struct S3 s = { a, b, c };
    return s;
}

static struct S9 bump_s9(struct S9 s) {
    s.x += 100;
    s.y = (char)(s.y + 1);
    return s;
}

static struct S24 add_s24(struct S24 p, struct S24 q) {
    struct S24 r = { p.a + q.a, p.b + q.b, p.c + q.c };
    return r;
}

static struct S64 scale_s64(struct S64 s, long long k) {
    for (int i = 0; i < 8; i++) {
        s.v[i] *= k;
    }
    return s;
}

static struct HFA3F add_hfa3f(struct HFA3F u, struct HFA3F v) {
    struct HFA3F r = { u.x + v.x, u.y + v.y, u.z + v.z };
    return r;
}

static struct HFA4D add_hfa4d(struct HFA4D u, struct HFA4D v) {
    struct HFA4D r = { u.a + v.a, u.b + v.b, u.c + v.c, u.d + v.d };
    return r;
}

static struct NonHFA5D sum_5d(struct NonHFA5D a, struct NonHFA5D b) {
    struct NonHFA5D r;
    for (int i = 0; i < 5; i++) {
        r.v[i] = a.v[i] + b.v[i];
    }
    return r;
}

/* K&R old-style function definition (valid in C99 §6.9.1) with float & char promotions */
static double knr_add(c, f)
    char c;
    float f;
{
    return (double)c + (double)f;
}

int main(void) {
    C99_BEGIN("c67_struct_union_abi", "Struct/union pass & return by value (1B-64B, HFA, K&R)");

    struct S3 s3 = make_s3(1, 2, 3);
    C99_CHECK_EQ(s3.a + s3.b + s3.c, 6);

    struct S9 s9 = bump_s9((struct S9){ 50, 'A' });
    C99_CHECK_EQ(s9.x, 150);
    C99_CHECK_EQ(s9.y, 'B');

    struct S24 (*fn24)(struct S24, struct S24) = add_s24;
    struct S24 r24 = fn24((struct S24){ 1, 2, 3 }, (struct S24){ 10, 20, 30 });
    C99_CHECK_EQ(r24.a, 11);
    C99_CHECK_EQ(r24.b, 22);
    C99_CHECK_EQ(r24.c, 33);

    struct S64 in64 = { { 1, 2, 3, 4, 5, 6, 7, 8 } };
    struct S64 out64 = scale_s64(in64, 3);
    C99_CHECK_EQ(out64.v[0], 3);
    C99_CHECK_EQ(out64.v[7], 24);
    /* Verify value semantics (in64 unmodified) */
    C99_CHECK_EQ(in64.v[7], 8);

    struct HFA3F hf = add_hfa3f((struct HFA3F){ 1.0f, 2.0f, 3.0f },
                                (struct HFA3F){ 0.5f, 1.5f, 2.5f });
    C99_CHECK_DBL(hf.x, 1.5, 0.0);
    C99_CHECK_DBL(hf.y, 3.5, 0.0);
    C99_CHECK_DBL(hf.z, 5.5, 0.0);

    struct HFA4D hd = add_hfa4d((struct HFA4D){ 1.0, 2.0, 3.0, 4.0 },
                                (struct HFA4D){ 10.0, 20.0, 30.0, 40.0 });
    C99_CHECK_DBL(hd.a + hd.b + hd.c + hd.d, 110.0, 0.0);

    struct NonHFA5D d5 = sum_5d((struct NonHFA5D){ { 1, 2, 3, 4, 5 } },
                                (struct NonHFA5D){ { 5, 4, 3, 2, 1 } });
    C99_CHECK_DBL(d5.v[0] + d5.v[4], 12.0, 0.0);

    /* K&R function call with default argument promotions */
    char ch = 10;
    float fl = 2.5f;
    C99_CHECK_DBL(knr_add(ch, fl), 12.5, 0.0);

    C99_END();
}
