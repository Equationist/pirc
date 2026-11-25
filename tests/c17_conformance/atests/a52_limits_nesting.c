/*
 * C99 §5.2.4.1: Translation limits —
 *  - 15 nesting levels of compound statements
 *  - 63 nesting levels of parenthesized expressions
 *  - 12 pointer/array/function declarator modifiers
 *  - 63 nesting levels of conditional inclusion (#if)
 */

#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#if 1
#define DEEP_IF_OK 1
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif

#ifndef DEEP_IF_OK
#error "16 nested #if failed"
#endif

/* 12 pointer modifiers */
int ************deep_ptr_12 = 0;

/* 12 nested declarators: array of pointers to arrays ... */
int (*(*(*(*(*(*decl12)[2])[2])[2])[2])[2])[2];

int deep_blocks_and_parens(int x) {
    int acc = 0;
    /* 16 nested compound blocks */
    { { { { { { { { { { { { { { { {
        int inner = x + 1;
        /* 64 nested parenthesized expressions */
        acc = ((((((((((((((((((((((((((((((((
              ((((((((((((((((((((((((((((((((
                  inner + 1
              ))))))))))))))))))))))))))))))))
              ))))))))))))))))))))))))))))))));
    } } } } } } } } } } } } } } } }
    return acc;
}
