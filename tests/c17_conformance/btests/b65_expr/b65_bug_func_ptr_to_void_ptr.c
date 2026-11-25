/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.16.1 p1 — Constraint violation:
 *   Implicit conversion between function pointer and object pointer (void *)
 *   is not permitted in ISO C17.
 */
void target_fn(void) {}

void *f(void) {
    void (*fp)(void) = target_fn;
    void *p = fp; /* ERROR: incompatible conversion from function ptr to void * */
    return p;
}
