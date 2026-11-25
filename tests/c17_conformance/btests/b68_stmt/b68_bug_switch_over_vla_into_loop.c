/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.4.2 — switch jumping over a VLA into a nested loop.
 */
int main(void) {
    int n = 3;
    switch (n) {
    case 0:;
        int vla[n];
        for (int i = 0; i < n; i++) {
        case 3:
            vla[0] = i;
            return vla[0];
        }
    }
    return 0;
}
