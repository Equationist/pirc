/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7p3 — Typedef redeclaration must denote the exact same type.
 */
typedef int A[];
typedef int A[3];
int main(void) { return 0; }
