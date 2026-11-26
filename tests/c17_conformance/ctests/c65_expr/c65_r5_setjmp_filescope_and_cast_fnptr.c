/* C17 §6.5.3.4, §6.5.2.2, §6.7.9:
 * - sizeof(setjmp(jb)) at file scope (fnctx == NULL must not crash)
 * - Calling function through cast function pointer with differing param type
 * - Static initializer copying a bit-field from a const struct must extract the bit-field */
#include "c17_test.h"
#include <setjmp.h>

static jmp_buf g_jb;
static size_t g_sj_sz = sizeof(setjmp(g_jb));

static int add_two(int a, int b) {
    return a + b;
}

typedef int (*TwoIntFn)(int, int);
typedef void (*VoidFn)(void);

struct BfBox {
    unsigned int pad : 5;
    unsigned int val : 7;
};
static const struct BfBox g_bf = { 13, 85 };

int main(void) {
    C17_CHECK(g_sj_sz == sizeof(int));

    /* Round-trip cast of function pointer via VoidFn */
    VoidFn vf = (VoidFn)add_two;
    TwoIntFn tf = (TwoIntFn)vf;
    C17_CHECK(tf(17, 25) == 42);

    /* Bit-field read from static const struct */
    C17_CHECK(g_bf.pad == 13);
    C17_CHECK(g_bf.val == 85);

    C17_END();
}
