/*
 * C17 §6.7.5, §6.5.3.4, §7.15 (<stdalign.h>), §7.19 (max_align_t):
 * _Alignas, _Alignof, alignas/alignof macros, and runtime address alignment.
 */
#include "c17_test.h"
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

_Alignas(64) static int g_aligned64 = 123;
alignas(32) static char g_aligned32 = 'Z';
_Alignas(double) static char g_aligned_dbl = 'D';

struct AlignedStruct {
    char tag;
    _Alignas(32) int payload;
};

int main(void) {
    C17_BEGIN("c67_alignas_alignof", "C17 _Alignas, _Alignof, <stdalign.h>, max_align_t");

    /* <stdalign.h> macros */
    C17_CHECK_EQ(__alignas_is_defined, 1);
    C17_CHECK_EQ(__alignof_is_defined, 1);

    /* _Alignof / alignof */
    C17_CHECK_EQ(_Alignof(char), 1);
    C17_CHECK_EQ(alignof(short), sizeof(short));
    C17_CHECK_EQ(alignof(int), sizeof(int));
    C17_CHECK_EQ(_Alignof(struct AlignedStruct), 32);
    C17_CHECK(_Alignof(max_align_t) >= _Alignof(double));
    C17_CHECK(_Alignof(max_align_t) >= _Alignof(long long));

    /* Global _Alignas runtime addresses */
    C17_CHECK_EQ((uintptr_t)&g_aligned64 % 64, 0);
    C17_CHECK_EQ((uintptr_t)&g_aligned32 % 32, 0);
    C17_CHECK_EQ((uintptr_t)&g_aligned_dbl % _Alignof(double), 0);
    C17_CHECK_EQ(g_aligned64, 123);
    C17_CHECK_EQ(g_aligned32, 'Z');

    /* Stack local _Alignas runtime addresses */
    _Alignas(16) int loc16 = 11;
    alignas(64) int loc64 = 22;
    _Alignas(double) char loc_dbl = 33;
    C17_CHECK_EQ((uintptr_t)&loc16 % 16, 0);
    C17_CHECK_EQ((uintptr_t)&loc64 % 64, 0);
    C17_CHECK_EQ((uintptr_t)&loc_dbl % _Alignof(double), 0);
    C17_CHECK_EQ(loc16 + loc64 + loc_dbl, 66);

    /* Struct member alignment & offset */
    struct AlignedStruct st = { 'A', 999 };
    C17_CHECK_EQ(offsetof(struct AlignedStruct, payload), 32);
    C17_CHECK_EQ((uintptr_t)&st.payload % 32, 0);
    C17_CHECK_EQ(st.payload, 999);

    C17_END();
}
