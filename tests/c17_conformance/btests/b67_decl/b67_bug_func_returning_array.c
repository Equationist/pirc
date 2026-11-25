/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.6.3 p1 — Constraint violation:
 *   A function declarator shall not specify a return type that is an array type.
 */
int bad_ret_arr(void)[4]; /* ERROR: function returning array */
