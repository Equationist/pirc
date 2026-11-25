/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.6p6 — Floating-point arithmetic in integer constant expression.
 */
enum { E = (int)(1.5 + 1.5) };
int main(void) { return E; }
