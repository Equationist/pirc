/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_TARGET_ABI_H
#define PIR_TARGET_ABI_H

#include <stdbool.h>
#include <stdint.h>

#include "ir/ir.h"

typedef enum {
    PIR_ABI_DIRECT = 0,     /* Passed/returned in 1..4 machine scalar/vec regs */
    PIR_ABI_INDIRECT        /* Passed/returned via pointer (caller copy or sret) */
} PirAbiClassKind;

typedef struct {
    PirAbiClassKind kind;
    PirType         regs[4];
    uint32_t        reg_count;
    uint32_t        gp_used;
    uint32_t        fp_used;
    uint32_t        pad_gp_before; /* Dummy i64 GP pads (e.g. odd-reg or all-or-nothing spill) */
    uint32_t        pad_fp_before; /* Dummy f64 FP pads (all-or-nothing HFA/HVA spill) */
    uint32_t        stack_align;   /* Stack slot alignment override when spilled (0 = default) */
    uint64_t        stack_byval_size;  /* SysV x64 MEMORY-class inline stack byval size */
    uint32_t        stack_byval_align; /* SysV x64 MEMORY-class inline stack byval alignment */
    bool            force_stack;   /* Force argument onto stack without consuming remaining regs */
} PirAbiArgInfo;

typedef struct {
    uint32_t gp_used;
    uint32_t fp_used;
} PirAbiRegState;

/* Classify a C ABI parameter (normal, byval, or i128) and update reg state. */
bool pir_abi_classify_param(const PirModule *mod, const PirTargetSpec *target,
                            const PirParamDecl *pd, PirAbiRegState *st,
                            PirAbiArgInfo *out);

/* Classify a C ABI retbuf(@T) return aggregate. */
bool pir_abi_classify_retbuf(const PirModule *mod, const PirTargetSpec *target,
                             PirType agg_ty, PirAbiArgInfo *out);

/* Target-specific classifiers */
bool pir_abi_sysv_x64_param(const PirModule *mod, const PirTargetSpec *target,
                            const PirParamDecl *pd, PirAbiRegState *st, PirAbiArgInfo *out);
bool pir_abi_sysv_x64_retbuf(const PirModule *mod, const PirTargetSpec *target,
                             PirType agg_ty, PirAbiArgInfo *out);

bool pir_abi_aapcs64_param(const PirModule *mod, const PirTargetSpec *target,
                           const PirParamDecl *pd, PirAbiRegState *st, PirAbiArgInfo *out);
bool pir_abi_aapcs64_retbuf(const PirModule *mod, const PirTargetSpec *target,
                            PirType agg_ty, PirAbiArgInfo *out);

bool pir_abi_win64_param(const PirModule *mod, const PirTargetSpec *target,
                         const PirParamDecl *pd, PirAbiRegState *st, PirAbiArgInfo *out);
bool pir_abi_win64_retbuf(const PirModule *mod, const PirTargetSpec *target,
                          PirType agg_ty, PirAbiArgInfo *out);

#endif /* PIR_TARGET_ABI_H */
