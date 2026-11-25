/* C17 §7.17: Cross-TU extern _Atomic variables (TU 1) */
#include "c17_test.h"
#include <stdatomic.h>

extern _Atomic int g_xtu_atom_i;
extern _Atomic long long g_xtu_atom_ll;
extern int tu2_atomic_bump(int delta, long long delta_ll);

int main(void) {
    C17_BEGIN("l67_pos_atomic_cross_tu", "Multi-TU extern _Atomic operations");
    C17_CHECK_EQ(atomic_load(&g_xtu_atom_i), 10);
    C17_CHECK_EQ(atomic_load(&g_xtu_atom_ll), 1000LL);

    int prev = tu2_atomic_bump(15, 250LL);
    C17_CHECK_EQ(prev, 10);
    C17_CHECK_EQ(atomic_load(&g_xtu_atom_i), 25);
    C17_CHECK_EQ(atomic_load(&g_xtu_atom_ll), 1250LL);

    int expected = 25;
    C17_CHECK_EQ(atomic_compare_exchange_strong(&g_xtu_atom_i, &expected, 99), 1);
    C17_CHECK_EQ(tu2_atomic_bump(1, 0LL), 99);
    C17_CHECK_EQ(atomic_load(&g_xtu_atom_i), 100);
    C17_END();
}
