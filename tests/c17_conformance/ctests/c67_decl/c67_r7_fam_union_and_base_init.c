/* C17 §6.7.2.1p3, §6.7.6.2, §6.7.9:
 * - Struct with flexible array member inside a union (valid in ISO C17)
 * - Static & automatic designated overrides on compound-literal/struct base initializers
 *   (including relocation override/removal and partial sub-object zeroing)
 * - Struct self-assignment (overlapping copy safety)
 * - Block-scope extern compatibility & VLA prototype parameter cleanup */
#include "c17_test.h"
#include <stddef.h>

struct FlexS {
    int n;
    int data[];
};

union FlexU {
    struct FlexS s;
    int tag;
};

static int g_a = 11, g_b = 22, g_c = 33;

struct PtrPair {
    int *p;
    int *q;
};

static struct PtrPair g_arr[2] = {
    [0] = (struct PtrPair){ &g_a, &g_b }, [0].p = 0,
    [1] = (struct PtrPair){ &g_a, &g_b }, [1].p = &g_c
};

struct Inner {
    int x;
    int y;
};

struct Outer {
    struct Inner in;
    int z;
    int w;
};

struct Big32 {
    long long v[4];
};

/* VLA prototype (non-definition): must not leak ast_arena nodes. */
void vla_proto_helper(int n, int a[n]);

void vla_proto_helper(int n, int a[n]) {
    if (n > 0) a[0] = 7;
}

static int ext_arr[3] = { 10, 20, 30 };

static int check_block_extern(void) {
    extern int ext_arr[];
    extern int ext_arr[3];
    return ext_arr[0] + ext_arr[1] + ext_arr[2];
}

int main(void) {
    /* 1. FAM struct in union */
    union FlexU fu;
    fu.tag = 0;
    fu.s.n = 123;
    C17_CHECK_EQ(fu.s.n, 123);
    C17_CHECK(sizeof(union FlexU) >= sizeof(int));

    /* 2. Static compound-literal base_expr with relocation override */
    C17_CHECK(g_arr[0].p == NULL);
    C17_CHECK(g_arr[0].q == &g_b && *g_arr[0].q == 22);
    C17_CHECK(g_arr[1].p == &g_c && *g_arr[1].p == 33);
    C17_CHECK(g_arr[1].q == &g_b && *g_arr[1].q == 22);

    /* 3. Automatic base_expr partial sub-object override zeros omitted fields */
    struct Outer base = { { 10, 20 }, 30, 40 };
    struct Outer o1[1] = { [0] = base, [0].in = { .x = 77 } };
    C17_CHECK_EQ(o1[0].in.x, 77);
    C17_CHECK_EQ(o1[0].in.y, 0);
    C17_CHECK_EQ(o1[0].z, 30);
    C17_CHECK_EQ(o1[0].w, 40);

    /* 4. Struct self-assignment (memmove safety) */
    struct Big32 b = { { 1, 2, 3, 4 } };
    b = b;
    C17_CHECK(b.v[0] == 1 && b.v[1] == 2 && b.v[2] == 3 && b.v[3] == 4);

    /* 5. VLA prototype + block-scope extern */
    int buf[2] = { 1, 2 };
    vla_proto_helper(2, buf);
    C17_CHECK_EQ(buf[0], 7);
    C17_CHECK_EQ(check_block_extern(), 60);

    C17_END();
}
