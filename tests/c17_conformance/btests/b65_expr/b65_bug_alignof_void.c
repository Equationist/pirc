/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.3.4p1 — _Alignof shall not be applied to an
 * incomplete type (void).
 * EXPECT-ERROR
 */
unsigned long f(void) {
    return _Alignof(void);
}
