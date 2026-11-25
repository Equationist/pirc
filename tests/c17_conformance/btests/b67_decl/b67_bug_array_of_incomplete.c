/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.6.2 p1 — Constraint violation:
 *   The element type of an array shall not be an incomplete type.
 */
struct Incomplete;

struct Incomplete bad_arr[5]; /* ERROR: array of incomplete type */
