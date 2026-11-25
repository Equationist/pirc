/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.1p3 — A structure containing a flexible array
 * member shall not be a member of a structure.
 * EXPECT-ERROR
 */
struct fam {
    int n;
    int a[];
};

struct wrap {
    struct fam f;
    int x;
};
