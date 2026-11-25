/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.5p5 — Block-scope compound literal has automatic duration.
 */
struct Pair { int a, b; };
int main(void) {
    static struct Pair *sp = &(struct Pair){ 1, 2 };
    return sp->a;
}
