/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.10.3p4 — Omitting comma for variadic macro argument in C17.
 */
#define F(a, ...) (a)
int main(void) { return F(0); }
