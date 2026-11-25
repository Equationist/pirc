/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.6.3p15 — Prototype parameter 'short' incompatible with K&R promoted 'int'.
 */
int f(short a);
int f(a)
    short a;
{
    return a;
}
int main(void) { return f(0); }
