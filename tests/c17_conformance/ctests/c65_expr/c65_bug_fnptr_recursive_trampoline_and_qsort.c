/*
 * C17 Conformance Test — Class C (§7.22.5 / §6.5.2.2)
 * Real-world C library callback ABI & indirect function-pointer state machine:
 *   1) Calling libc qsort() and bsearch() with slimcc-pir-compiled comparators.
 *   2) Recursive state machine returning next-state function pointer wrapped
 *      in a struct.
 */
#include "c17_test.h"
#include <stdint.h>
#include <stdlib.h>

struct Item {
    int32_t key;
    int32_t payload;
};

static int cmp_item_asc(const void *pa, const void *pb) {
    const struct Item *a = (const struct Item *)pa;
    const struct Item *b = (const struct Item *)pb;
    if (a->key < b->key) return -1;
    if (a->key > b->key) return 1;
    return (a->payload > b->payload) - (a->payload < b->payload);
}

struct State;
typedef struct State (*StateFn)(int *acc, int step);
struct State {
    StateFn next;
};

static struct State st_a(int *acc, int step);
static struct State st_b(int *acc, int step);
static struct State st_c(int *acc, int step);

static struct State st_a(int *acc, int step) {
    *acc += step * 2;
    struct State s = { (step & 1) ? st_b : st_c };
    return s;
}

static struct State st_b(int *acc, int step) {
    *acc -= step;
    struct State s = { st_c };
    return s;
}

static struct State st_c(int *acc, int step) {
    *acc ^= (step * 7);
    struct State s = { st_a };
    return s;
}

int main(void) {
    C17_BEGIN("c65_bug_fnptr_recursive_trampoline_and_qsort",
              "libc qsort/bsearch callbacks and struct-wrapped fn-ptr state machine");

    /* 1. libc qsort + bsearch with slimcc-pir comparator */
    struct Item items[8] = {
        { 40, 1 }, { -5, 2 }, { 12, 9 }, { 0, 3 },
        { 12, 4 }, { 99, 5 }, { -20, 6 }, { 7, 7 }
    };
    qsort(items, 8, sizeof(items[0]), cmp_item_asc);
    C17_CHECK(items[0].key == -20 && items[1].key == -5 && items[2].key == 0);
    C17_CHECK(items[3].key == 7);
    C17_CHECK(items[4].key == 12 && items[4].payload == 4);
    C17_CHECK(items[5].key == 12 && items[5].payload == 9);
    C17_CHECK(items[6].key == 40 && items[7].key == 99);

    struct Item needle = { 12, 9 };
    struct Item *found = (struct Item *)bsearch(&needle, items, 8, sizeof(items[0]), cmp_item_asc);
    C17_CHECK(found != NULL && found == &items[5]);

    struct Item missing = { 13, 0 };
    C17_CHECK(bsearch(&missing, items, 8, sizeof(items[0]), cmp_item_asc) == NULL);

    /* 2. Struct-wrapped function-pointer state machine */
    struct State st = { st_a };
    int acc = 0;
    for (int i = 1; i <= 10; i++) {
        st = st.next(&acc, i);
    }
    /* Independent trace:
     * i=1: st_a -> acc=2, next=st_b
     * i=2: st_b -> acc=0, next=st_c
     * i=3: st_c -> acc=0^21=21, next=st_a
     * i=4: st_a -> acc=29, next=st_c
     * i=5: st_c -> acc=29^35=62, next=st_a
     * i=6: st_a -> acc=74, next=st_c
     * i=7: st_c -> acc=74^49=123, next=st_a
     * i=8: st_a -> acc=139, next=st_c
     * i=9: st_c -> acc=139^63=180, next=st_a
     * i=10: st_a -> acc=200, next=st_c */
    C17_CHECK(acc == 200);
    C17_CHECK(st.next == st_c);

    C17_END();
}
