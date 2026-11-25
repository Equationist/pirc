/* C17 §7.17: Cross-TU extern _Atomic variables (TU 2) */
#include <stdatomic.h>

_Atomic int g_xtu_atom_i = 10;
_Atomic long long g_xtu_atom_ll = 1000LL;

int tu2_atomic_bump(int delta, long long delta_ll) {
    atomic_fetch_add(&g_xtu_atom_ll, delta_ll);
    return atomic_fetch_add(&g_xtu_atom_i, delta);
}
