/*
 * C99 §6.7.2.1 & §6.5.2.3 p5: Self-referential and mutually recursive structs,
 * incomplete struct pointers in prototypes, union common initial sequence,
 * and union type punning.
 */
#include "c17_test.h"
#include <stdint.h>

/* Mutually recursive structs + forward pointer prototype */
struct NodeA;
struct NodeB;

static int sum_chain(const struct NodeA *a);

struct NodeB {
    int val;
    const struct NodeA *next_a;
};

struct NodeA {
    int val;
    const struct NodeB *next_b;
};

static int sum_chain(const struct NodeA *a) {
    int s = 0;
    while (a) {
        s += a->val;
        if (a->next_b) {
            s += a->next_b->val;
            a = a->next_b->next_a;
        } else {
            break;
        }
    }
    return s;
}

/* §6.5.2.3 p5: Union of structs sharing a common initial sequence */
struct KindA { int kind; int payload_i; };
struct KindB { int kind; double payload_d; };
union Variant {
    struct KindA a;
    struct KindB b;
};

static int get_kind(const union Variant *v) {
    /* Reading v->a.kind is valid even when v->b was written (common initial sequence) */
    return v->a.kind;
}

/* Union type punning (explicitly permitted in C99 TC3 §6.5.2.3 footnote 82) */
union FloatBits {
    float f;
    uint32_t u;
};

int main(void) {
    C99_BEGIN("c67_recursive_structs_unions", "Mutually recursive structs, common initial seq, union punning");

    struct NodeA a2 = { 30, NULL };
    struct NodeB b1 = { 20, &a2 };
    struct NodeA a1 = { 10, &b1 };
    C99_CHECK_EQ(sum_chain(&a1), 60);

    union Variant v;
    v.b.kind = 2;
    v.b.payload_d = 3.5;
    C99_CHECK_EQ(get_kind(&v), 2);
    C99_CHECK_DBL(v.b.payload_d, 3.5, 0.0);

    union FloatBits fb;
    fb.f = -2.0f;
    /* IEEE 754 float -2.0f = 0xC0000000 */
    C99_CHECK_UEQ(fb.u, 0xC0000000u);
    fb.u = 0x3F800000u; /* +1.0f */
    C99_CHECK_DBL(fb.f, 1.0, 0.0);

    C99_END();
}
