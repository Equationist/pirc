/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_TARGET_TARGET_H
#define PIR_TARGET_TARGET_H

#include <stdbool.h>

#include "ir/ir.h"

bool        pir_target_from_triple(const char *triple, PirTargetSpec *out);
bool        pir_target_apply_caps(PirTargetSpec *spec, const char *profile);
const char *pir_target_elf_builtin_section_dir(const char *name);
bool        pir_target_is_elf_reserved_sym(const PirTargetSpec *target, const char *name);
bool        pir_target_section_is_nobits(const PirTargetSpec *target, const char *sec);
const char *pir_target_validate_section(const PirTargetSpec *target, const char *sec);
bool        pir_target_macho_seg_sec_key(const char *sec, char *buf, size_t cap);

#endif /* PIR_TARGET_TARGET_H */
