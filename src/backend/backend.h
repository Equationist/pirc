/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_BACKEND_BACKEND_H
#define PIR_BACKEND_BACKEND_H

#include <stdbool.h>
#include <stdio.h>

#include "backend/mir.h"
#include "common/diag.h"
#include "ir/ir.h"

/* =========================================================================
 * CodeEmitter Vtable (Shared between Phase 1 ASM and M9 Direct Obj Emitters)
 * ========================================================================= */

typedef struct PirCodeEmitter PirCodeEmitter;

struct PirCodeEmitter {
    void *ctx;
    bool (*emit_module_header)(PirCodeEmitter *em, const PirModule *mod,
                               const PirTargetSpec *target);
    bool (*emit_function)(PirCodeEmitter *em, const PirModule *mod, const MFunc *mf);
    bool (*emit_globals)(PirCodeEmitter *em, const PirModule *mod,
                         const PirTargetSpec *target);
    bool (*emit_module_footer)(PirCodeEmitter *em, const PirModule *mod,
                               const PirTargetSpec *target);
};

/* Register allocation (target-configured linear scan + parallel copy sequentializer) */
bool pir_regalloc_run(MFunc *mf, PirDiag *diag);

#ifdef PIR_VERIFY_ALL
/* MIR structural verifiers (pre-regalloc and post-regalloc) */
bool pir_mir_verify_pre_ra(const MFunc *mf, PirDiag *diag);
bool pir_mir_verify_post_ra(const MFunc *mf, PirDiag *diag);
#endif

/* ARM64 backend passes (M6) */
bool pir_arm64_isel_func(const PirModule *mod, const PirFunction *fn, uint32_t fn_index,
                         const PirTargetSpec *target, MFunc *out_mf, PirDiag *diag);
bool pir_arm64_frame_lower(MFunc *mf, PirDiag *diag);
void pir_arm64_asm_emitter_init(PirCodeEmitter *em, FILE *out, const PirTargetSpec *target);

/* x86-64 backend passes (M8) */
bool pir_x64_isel_func(const PirModule *mod, const PirFunction *fn, uint32_t fn_index,
                       const PirTargetSpec *target, MFunc *out_mf, PirDiag *diag);
bool pir_x64_frame_lower(MFunc *mf, PirDiag *diag);
void pir_x64_asm_emitter_init(PirCodeEmitter *em, FILE *out, const PirTargetSpec *target);

/* RISC-V 32-bit (RV32IMAFD) backend passes */
bool pir_rv32_isel_func(const PirModule *mod, const PirFunction *fn, uint32_t fn_index,
                        const PirTargetSpec *target, MFunc *out_mf, PirDiag *diag);
bool pir_rv32_frame_lower(MFunc *mf, PirDiag *diag);
void pir_rv32_asm_emitter_init(PirCodeEmitter *em, FILE *out, const PirTargetSpec *target);

/* Top-level backend driver: lowers every defined function to post-regalloc MFunc
 * and streams through the selected CodeEmitter. */
bool pir_backend_emit_asm(const PirModule *mod, const PirTargetSpec *target,
                          FILE *out, PirDiag *diag);

#endif /* PIR_BACKEND_BACKEND_H */
