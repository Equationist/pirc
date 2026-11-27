/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "common/diag.h"

#include <stdarg.h>
#include <stdio.h>

void pir_diag_init(PirDiag *diag, const char *filename) {
    diag->filename    = filename ? filename : "<input>";
    diag->error_count = 0u;
}

#define PIR_DIAG_MAX_PRINTED 100u

void pir_diag_error(PirDiag *diag, uint32_t line, uint32_t col, const char *fmt, ...) {
    if (diag->error_count < PIR_DIAG_MAX_PRINTED) {
        va_list ap;
        if (line > 0u && col > 0u) {
            fprintf(stderr, "%s:%u:%u: error: ", diag->filename, line, col);
        } else if (line > 0u) {
            fprintf(stderr, "%s:%u: error: ", diag->filename, line);
        } else {
            fprintf(stderr, "%s: error: ", diag->filename);
        }
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    } else if (diag->error_count == PIR_DIAG_MAX_PRINTED) {
        fprintf(stderr, "%s: note: too many errors; suppressing further diagnostics\n",
                diag->filename);
    }
    if (diag->error_count < UINT32_MAX) {
        diag->error_count++;
    }
}
