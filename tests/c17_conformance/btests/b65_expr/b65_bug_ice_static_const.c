/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.6p6 — static const variable is not an integer constant expression.
 */
static const int N = 4;
enum { E = N };
int main(void) { return E; }
