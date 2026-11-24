/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_COMMON_DIAG_H
#define PIR_COMMON_DIAG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *filename;
    uint32_t    error_count;
} PirDiag;

void pir_diag_init(PirDiag *diag, const char *filename);
void pir_diag_error(PirDiag *diag, uint32_t line, uint32_t col, const char *fmt, ...);

static inline bool pir_diag_has_errors(const PirDiag *diag) {
    return diag->error_count > 0u;
}

#endif /* PIR_COMMON_DIAG_H */
