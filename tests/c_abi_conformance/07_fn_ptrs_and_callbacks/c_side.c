/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "common.h"

typedef struct { int32_t key; int32_t payload; } Item;
typedef Item (*TransformFn)(Item in, int32_t scale);
typedef int32_t (*ScoreFn)(Item in);

/* Implemented in PIR */
extern void    pir_sort_items_via_qsort(Item *arr, int64_t n);
extern Item    pir_invoke_c_transform(TransformFn fn, Item in, int32_t scale);
extern ScoreFn pir_get_score_callback(void);

/* Implemented in C, passed as fn ptr to PIR */
static Item c_scale_item(Item in, int32_t scale) {
    return (Item){ in.key * scale, in.payload + scale };
}

int main(void) {
    /* 1. PIR calls host libc qsort() with a PIR comparator callback */
    Item arr[6] = {
        { 40, 1 }, { 10, 2 }, { 60, 3 },
        { 20, 4 }, { 50, 5 }, { 30, 6 }
    };
    pir_sort_items_via_qsort(arr, 6);
    CHECK_EQ_I64(arr[0].key, 10); CHECK_EQ_I64(arr[0].payload, 2);
    CHECK_EQ_I64(arr[1].key, 20);
    CHECK_EQ_I64(arr[2].key, 30);
    CHECK_EQ_I64(arr[3].key, 40);
    CHECK_EQ_I64(arr[4].key, 50);
    CHECK_EQ_I64(arr[5].key, 60); CHECK_EQ_I64(arr[5].payload, 3);

    /* 2. C passes C fn ptr to PIR -> PIR executes call_indirect @Sig with byval+retbuf */
    Item res = pir_invoke_c_transform(c_scale_item, (Item){ 7, 100 }, 3);
    CHECK_EQ_I64(res.key, 21);
    CHECK_EQ_I64(res.payload, 103);

    /* 3. PIR returns a function pointer to C -> C invokes it directly */
    ScoreFn sc = pir_get_score_callback();
    CHECK(sc != NULL);
    CHECK_EQ_I64(sc((Item){ 10, 5 }), 25);

    printf("PASS\n");
    return 0;
}
