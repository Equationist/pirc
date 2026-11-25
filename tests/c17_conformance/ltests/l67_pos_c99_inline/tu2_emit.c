/* C99 §6.7.4 p6: C99 inline linkage across TUs (TU 2 - external def emitter) */
#include "inline_hdr.h"

/* Emits the external definitions for c99_inl_add and c99_inl_mul */
extern inline int c99_inl_add(int a, int b);
extern inline int c99_inl_mul(int a, int b);

int call_from_tu2(int x, int y) {
    return c99_inl_add(x, y) + c99_inl_mul(x, y);
}
