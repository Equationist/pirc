/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_ANALYSIS_ALIAS_H
#define PIR_ANALYSIS_ALIAS_H

#include <stdbool.h>
#include <stdint.h>

#include "ir/ir.h"

typedef enum {
    PIR_ALIAS_NO = 0,
    PIR_ALIAS_MAY,
    PIR_ALIAS_MUST
} PirAliasResult;

typedef enum {
    PIR_PTR_UNKNOWN = 0,
    PIR_PTR_ALLOCA,
    PIR_PTR_GLOBAL,
    PIR_PTR_PARAM_NOALIAS,
    PIR_PTR_PARAM
} PirPtrBaseKind;

typedef struct {
    PirPtrBaseKind kind;
    uint32_t       obj_id;     /* InstId for alloca, ParamIdx for param */
    const char    *global_name;/* Symbol name for global */
    int64_t        offset;     /* Constant byte offset (sum of constant ptr_add steps) */
    uint64_t       stride;     /* GCD of variable step multipliers when !has_offset (0/1 = unknown) */
    bool           has_offset; /* True if offset is statically known */
} PirPtrInfo;

PirPtrInfo     pir_alias_decompose(const PirFunction *fn, PirOperand op);
PirAliasResult pir_alias_query(const PirFunction *fn,
                               PirOperand p1, uint32_t sz1,
                               PirOperand p2, uint32_t sz2);

#endif /* PIR_ANALYSIS_ALIAS_H */
