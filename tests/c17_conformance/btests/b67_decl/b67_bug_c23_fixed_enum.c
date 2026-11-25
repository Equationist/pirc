/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.2 — Fixed underlying enum type is a C23 extension.
 */
enum E : short { A = 1 };
int main(void) { return A; }
