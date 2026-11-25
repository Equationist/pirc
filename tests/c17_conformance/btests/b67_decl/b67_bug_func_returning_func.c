/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.6.3 p1 — Constraint violation:
 *   A function declarator shall not specify a return type that is a function type.
 */
int (bad_ret_fn(void))(int); /* ERROR: function returning function */
