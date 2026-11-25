/*
 * C17 §6.5: Aliasing, optimizer honesty, and large stack frame (>16 KB Darwin
 * stack probe) stress test.
 */
#include "c17_test.h"
#include <stdint.h>

struct Box {
    int *ptr;
};

static void indirect_write(struct Box *b, int v) {
    *(b->ptr) = v;
}

static int may_alias_store(int *p, int *q) {
    *p = 11;
    *q = 22;
    return *p; /* Must return 22 when p == q, 11 when p != q! */
}

static void swap_ints(int *a, int *b) {
    int t = *a;
    *a = *b;
    *b = t;
}

/* > 16 KB local stack frame exercises ___chkstk_darwin and >4095 frame offsets */
static int large_frame_checksum(int seed) {
    unsigned char buf[24576];
    for (int i = 0; i < 24576; i += 256) {
        buf[i] = (unsigned char)((seed + i) & 0xFF);
    }
    buf[0] = (unsigned char)(seed & 0xFF);
    buf[24575] = (unsigned char)((seed + 7) & 0xFF);
    return buf[0] + buf[4096] + buf[16384] + buf[24575];
}

int main(void) {
    C17_BEGIN("c65_aliasing_and_opt_traps", "May-alias stores, escaped locals, memcpy pun, >16KB stack frame");

    /* Escaped local pointer mutated via indirect function call */
    int escaped = 5;
    struct Box box = { &escaped };
    indirect_write(&box, 77);
    C17_CHECK_EQ(escaped, 77);

    /* May-alias pointer store test (same vs distinct addresses) */
    int arr[2] = { 0, 0 };
    int i0 = c17_opaque_int(0);
    int i1 = c17_opaque_int(1);
    C17_CHECK_EQ(may_alias_store(&arr[i0], &arr[i0]), 22);
    C17_CHECK_EQ(may_alias_store(&arr[i0], &arr[i1]), 11);

    /* Self-swap and self-struct assignment */
    int sv = 123;
    swap_ints(&sv, &sv);
    C17_CHECK_EQ(sv, 123);

    /* memcpy type-punning inside loop */
    uint64_t sum_bits = 0;
    for (int k = 1; k <= 4; k++) {
        double d = (double)k;
        uint64_t u = 0;
        memcpy(&u, &d, sizeof(u));
        sum_bits ^= u;
    }
    C17_CHECK(sum_bits != 0);

    /* Volatile inside loop must not be hoisted/elided */
    volatile int sink = 0;
    int reads = 0;
    for (int k = 1; k <= 5; k++) {
        sink = k;
        reads += sink;
    }
    C17_CHECK_EQ(reads, 15);

    /* >16 KB stack frame */
    int s = c17_opaque_int(10);
    C17_CHECK_EQ(large_frame_checksum(s), 10 + 10 + 10 + 17);

    C17_END();
}
