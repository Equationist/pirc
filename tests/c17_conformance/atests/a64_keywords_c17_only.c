/*
 * C17 §6.4.1: Strict C17 keyword set.
 * C23 keywords (alignas, alignof, static_assert, thread_local, constexpr,
 * nullptr, typeof, bool, true, false) must remain usable as ordinary
 * identifiers in strict C17 mode when no library header defines them.
 */

int strict_c17_identifiers(void) {
    int alignas = 1;
    int alignof = 2;
    int static_assert = 3;
    int thread_local = 4;
    int constexpr = 5;
    int nullptr = 6;
    int typeof = 7;
    int bool = 8;
    int true = 9;
    int false = 10;
    int asm = 11;
    return alignas + alignof + static_assert + thread_local +
           constexpr + nullptr + typeof + bool + true + false + asm;
}
