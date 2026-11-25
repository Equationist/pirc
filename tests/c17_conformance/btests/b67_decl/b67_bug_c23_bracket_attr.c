/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) — Standard [[]] attributes are a C23 extension.
 */
[[nodiscard]] int f(void) { return 0; }
int main(void) { return f(); }
