/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_IR_PRINTER_H
#define PIR_IR_PRINTER_H

#include <stdio.h>

#include "ir/ir.h"

void pir_print_module(const PirModule *mod, FILE *out);
void pir_print_type(const PirModule *mod, PirType ty, FILE *out);

#endif /* PIR_IR_PRINTER_H */
