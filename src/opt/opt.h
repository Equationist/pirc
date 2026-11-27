/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_OPT_OPT_H
#define PIR_OPT_OPT_H

#include <stdbool.h>

#include "common/diag.h"
#include "ir/ir.h"

/* Run LIR optimization pipeline at opt_level (0, 1, or 2). */
bool pir_opt_module(PirModule *mod, const PirTargetSpec *target, int opt_level, PirDiag *diag);

/* Individual LIR optimization passes */
void pir_opt_inline(PirModule *mod, const PirTargetSpec *target);
void pir_opt_simplify_cfg(PirModule *mod, PirFunction *fn);
void pir_opt_edge_split(PirModule *mod, PirFunction *fn);
void pir_opt_mem2reg(PirModule *mod, PirFunction *fn, const PirTargetSpec *target);
void pir_opt_gvn(PirModule *mod, PirFunction *fn, const PirTargetSpec *target);
void pir_opt_licm(PirModule *mod, PirFunction *fn, const PirTargetSpec *target);
void pir_opt_dse(PirModule *mod, PirFunction *fn, const PirTargetSpec *target);
void pir_opt_dce(PirModule *mod, PirFunction *fn);
void pir_opt_loop_vec(PirModule *mod, PirFunction *fn, const PirTargetSpec *target);
void pir_opt_loop_rotate(PirModule *mod, PirFunction *fn);

/* Centralized instruction effect flags (single source of truth for all opt passes) */
enum {
    PIR_EFF_NONE           = 0u,
    PIR_EFF_MAY_TRAP       = 1u << 0, /* Can trap, exit, unwind, or call weak-TLS thunk */
    PIR_EFF_MAY_READ_MEM   = 1u << 1, /* Reads memory */
    PIR_EFF_MAY_WRITE_MEM  = 1u << 2, /* Writes memory or mutates stack/va_list state */
    PIR_EFF_MEM_BARRIER    = 1u << 3, /* Volatile, atomic, fence, bulk mem, or opaque call */
    PIR_EFF_REAL_CALL      = 1u << 4, /* Real call or TLS thunk (clobbers regs/state) */
    PIR_EFF_CONTROL_FLOW   = 1u << 5, /* Block terminator */
    PIR_EFF_PURE_HOISTABLE = 1u << 6, /* Total, side-effect-free, speculatively hoistable */
    PIR_EFF_HAS_UB_FLAG    = 1u << 7  /* Carries [trap*]/[unsafe*]/[nonzero] semantics */
};

/* Shared optimizer predicates */
bool     pir_op_is_tls_sym(const PirModule *mod, const PirOperand *op);
uint32_t pir_inst_effects(const PirModule *mod, const PirFunction *fn, const PirInst *ins);
bool     pir_inst_is_pure_math_call(const PirModule *mod, const PirFunction *fn, const PirInst *ins);
bool     pir_is_returns_twice_name(const char *name);
bool     pir_fn_has_returns_twice(const PirFunction *fn);
bool     pir_operand_is_proven_bool_i8(const PirFunction *fn, PirOperand op);
bool     pir_operand_eq_exact(PirOperand a, PirOperand b);

#endif /* PIR_OPT_OPT_H */
