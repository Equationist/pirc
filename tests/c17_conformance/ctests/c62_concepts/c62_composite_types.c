/* C99-REF-CFLAGS: -Wno-strict-prototypes */
/*
 * C99 §6.2.7 & §6.5 p7: Compatible/composite types, tentative definitions,
 * K&R/prototype merging, and object representation via unsigned char.
 */
#include "c17_test.h"
#include <stdint.h>

/* Incomplete array completed later -> composite type int[5] */
extern int comp_arr[];
int comp_arr[5] = { 10, 20, 30, 40, 50 };

/* Tentative definitions merged */
int tent_x;
int tent_x;
int tent_x = 77;
int tent_x;

/* Non-prototype declaration merged with prototype */
static int merge_fn();
static int merge_fn(int a, double b);
static int merge_fn(int a, double b) {
    return a + (int)(b * 10.0);
}

/* Pointer to incomplete array compatible with pointer to complete array */
static int sum_row(int (*p)[3]) {
    return (*p)[0] + (*p)[1] + (*p)[2];
}

int main(void) {
    C99_BEGIN("c62_composite_types", "Composite types, tentative defs, object representation");

    C99_CHECK_EQ(sizeof(comp_arr), 5 * sizeof(int));
    C99_CHECK_EQ(comp_arr[4], 50);
    C99_CHECK_EQ(tent_x, 77);
    C99_CHECK_EQ(merge_fn(5, 2.5), 30);

    int row[3] = { 3, 6, 9 };
    int (*p_inc)[] = &row;
    int (*p_comp)[3] = p_inc;
    C99_CHECK_EQ(sum_row(p_comp), 18);

    /* Object representation inspection via unsigned char (always allowed by §6.5 p7) */
    uint32_t val = 0x11223344u;
    unsigned char buf[4];
    memcpy(buf, &val, 4);
    uint32_t reconstructed = 0;
    memcpy(&reconstructed, buf, 4);
    C99_CHECK_UEQ(reconstructed, 0x11223344u);

    /* Check unsigned char aliasing read/write */
    unsigned char *bytes = (unsigned char *)&val;
    unsigned char saved = bytes[0];
    bytes[0] = 0x55u;
    C99_CHECK(val != 0x11223344u);
    bytes[0] = saved;
    C99_CHECK_UEQ(val, 0x11223344u);

    C99_END();
}
