/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.6.3p15 — Function redeclaration with mismatched return qualification.
 */
const int f(void);
int f(void);
int main(void) { return 0; }
