/*
 * C17 Conformance Test — Class C (§6.5.2.3p6 / §6.6 / §6.2.4)
 * Adversarial regressions for:
 *   1) Union common initial sequence (§6.5.2.3p6) inspected through non-active
 *      union member with identical prefix layout.
 *   2) Static pointer arithmetic relocations (&arr[5] - 3, &arr[8]), cyclic
 *      self-referential static struct chain, and static const fn-ptr table.
 *   3) Initialized _Thread_local struct and array globals.
 */
#include "c17_test.h"
#include <stdint.h>

struct HdrA { int tag; int32_t len; double payload; };
struct HdrB { int tag; int32_t len; int64_t extra; };
union Packet {
    struct HdrA a;
    struct HdrB b;
};

static int read_common_tag(const union Packet *u) {
    return u->b.tag * 100 + u->b.len;
}

static int g_table[10] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
static int *const g_ptr_rel2 = &g_table[5] - 3; /* points to g_table[2] == 30 */
static int *const g_ptr_rel8 = g_table + 8;     /* points to g_table[8] == 90 */

struct RingNode {
    int val;
    const struct RingNode *next;
};
static const struct RingNode r_n1, r_n2, r_n3;
static const struct RingNode r_n1 = { 11, &r_n2 };
static const struct RingNode r_n2 = { 22, &r_n3 };
static const struct RingNode r_n3 = { 33, &r_n1 };

static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }

struct DispatchEntry {
    const char *name;
    int (*fn)(int, int);
};

static const struct DispatchEntry g_dispatch[3] = {
    { "add", op_add },
    { "sub", op_sub },
    { "mul", op_mul },
};

struct TlsState {
    int magic;
    int arr[4];
};
static _Thread_local struct TlsState g_tls = { 42, { 1, 2, 3, 4 } };

int main(void) {
    C17_BEGIN("c65_bug_common_initial_seq_and_static_relocs",
              "Union common initial sequence, static ptr relocs, cyclic ring, TLS struct");

    /* 1. Union common initial sequence */
    union Packet pkt;
    pkt.a.tag = 7;
    pkt.a.len = 25;
    pkt.a.payload = 3.5;
    C17_CHECK(read_common_tag(&pkt) == 725);

    /* 2. Static pointer relocations */
    C17_CHECK(*g_ptr_rel2 == 30);
    C17_CHECK(g_ptr_rel2[1] == 40);
    C17_CHECK(*g_ptr_rel8 == 90);
    C17_CHECK(g_ptr_rel8[-1] == 80);

    /* 3. Cyclic self-referential static ring */
    const struct RingNode *cur = &r_n1;
    int ring_sum = 0;
    for (int i = 0; i < 6; i++) {
        ring_sum += cur->val;
        cur = cur->next;
    }
    C17_CHECK(ring_sum == 2 * (11 + 22 + 33));
    C17_CHECK(cur == &r_n1);

    /* 4. Static const function-pointer dispatch table */
    int x = c17_opaque_int(12), y = c17_opaque_int(5);
    C17_CHECK(g_dispatch[0].fn(x, y) == 17);
    C17_CHECK(g_dispatch[1].fn(x, y) == 7);
    C17_CHECK(g_dispatch[2].fn(x, y) == 60);

    /* 5. Initialized _Thread_local struct & array */
    C17_CHECK(g_tls.magic == 42);
    C17_CHECK(g_tls.arr[0] + g_tls.arr[1] + g_tls.arr[2] + g_tls.arr[3] == 10);
    g_tls.arr[2] += 100;
    C17_CHECK(g_tls.arr[2] == 103);

    C17_END();
}
