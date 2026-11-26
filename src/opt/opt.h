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

/* Shared optimizer predicates */
bool pir_inst_is_pure_math_call(const PirModule *mod, const PirFunction *fn, const PirInst *ins);
bool pir_is_returns_twice_name(const char *name);
bool pir_fn_has_returns_twice(const PirFunction *fn);
bool pir_operand_is_proven_bool_i8(const PirFunction *fn, PirOperand op);
bool pir_operand_eq_exact(PirOperand a, PirOperand b);

#endif /* PIR_OPT_OPT_H */
