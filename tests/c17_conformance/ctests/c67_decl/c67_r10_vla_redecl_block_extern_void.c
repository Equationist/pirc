/* Round 10 C17 conformance regressions:
 * - VLA parameter redeclaration inside function body preserves live VLA length
 * - Block-scope extern array composite type within same & nested blocks
 * - extern void object declaration (C17 6.9.2 / 6.7)
 * - Ternary void * + _Atomic int * strips _Atomic (C17 6.5.15p6)
 * - #pragma pack macro argument expansion
 */
#include "c17_test.h"
#include <stddef.h>

int vla_fn(int n, int a[n]) {
    int vla_fn(int n, int a[n]);
    return (int)sizeof(a[0]) + n + a[n - 1];
}

int arr_glob[4] = {10, 20, 30, 40};
static int test_block_extern(void) {
    int s1 = 0, s2 = 0;
    {
        extern int arr_glob[];
        extern int arr_glob[4];
        s1 = (int)sizeof(arr_glob);
        {
            extern int arr_glob[];
            s2 = (int)sizeof(arr_glob);
        }
    }
    return s1 == 4 * (int)sizeof(int) && s2 == 4 * (int)sizeof(int);
}

extern void r10_unused_extern_void;

static int test_ternary_atomic(void) {
    _Atomic int x = 7;
    void *vp = 0;
    const void *cvp = 0;
    _Atomic int *ap = &x;
    int r1 = _Generic(1 ? vp : ap, void *: 1, default: 0);
    int r2 = _Generic(1 ? cvp : ap, const void *: 1, default: 0);
    return r1 && r2;
}

#define R10_PACK_ALIGN 1
#pragma pack(push, R10_PACK_ALIGN)
struct R10Packed {
    char a;
    int b;
};
#pragma pack(pop)

int main(void) {
    int arr[3] = {5, 6, 7};
    C17_CHECK(vla_fn(3, arr) == (int)sizeof(int) + 3 + 7);
    C17_CHECK(test_block_extern());
    C17_CHECK(test_ternary_atomic());
    C17_CHECK(sizeof(struct R10Packed) == sizeof(char) + sizeof(int));
    C17_END();
}
