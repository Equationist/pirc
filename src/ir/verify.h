/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_IR_VERIFY_H
#define PIR_IR_VERIFY_H

#include <stdbool.h>

#include "common/diag.h"
#include "ir/ir.h"

bool pir_verify_module(const PirModule *mod, PirVerifyMode mode,
                       const PirTargetSpec *target, PirDiag *diag);

#endif /* PIR_IR_VERIFY_H */
