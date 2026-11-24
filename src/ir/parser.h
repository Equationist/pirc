/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_IR_PARSER_H
#define PIR_IR_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#include "common/diag.h"
#include "ir/ir.h"

bool pir_parse_module(const char *src, size_t len, const char *filename,
                      PirModule *mod, PirDiag *diag);

#endif /* PIR_IR_PARSER_H */
