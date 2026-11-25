/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.10p2 — Empty macro before '#' does not start a directive.
 */
#define EMPTY
EMPTY #define FOO 1
int main(void) { return FOO; }
