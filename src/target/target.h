/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_TARGET_TARGET_H
#define PIR_TARGET_TARGET_H

#include <stdbool.h>

#include "ir/ir.h"

bool pir_target_from_triple(const char *triple, PirTargetSpec *out);
bool pir_target_apply_caps(PirTargetSpec *spec, const char *profile);

#endif /* PIR_TARGET_TARGET_H */
