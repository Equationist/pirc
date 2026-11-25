#ifndef INLINE_HDR_H
#define INLINE_HDR_H

/* C99 §6.7.4 p6: inline definition (does NOT emit external definition by itself) */
inline int c99_inl_add(int a, int b) {
    return a + b;
}

inline int c99_inl_mul(int a, int b) {
    return a * b;
}

#endif
