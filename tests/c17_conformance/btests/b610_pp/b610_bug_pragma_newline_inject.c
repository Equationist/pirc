/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.10.9p1 — _Pragma destringization must not turn '\n' into a newline.
 */
_Pragma("unknown\nint injected_var = 42;")
int main(void) {
    return injected_var;
}
