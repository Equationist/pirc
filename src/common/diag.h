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

#if defined(__GNUC__) || defined(__clang__)
#define PIR_PRINTF_LIKE(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define PIR_PRINTF_LIKE(fmt_idx, arg_idx)
#endif

void pir_diag_init(PirDiag *diag, const char *filename);
void pir_diag_error(PirDiag *diag, uint32_t line, uint32_t col, const char *fmt, ...)
    PIR_PRINTF_LIKE(4, 5);

static inline bool pir_diag_has_errors(const PirDiag *diag) {
    return diag->error_count > 0u;
}

#endif /* PIR_COMMON_DIAG_H */
