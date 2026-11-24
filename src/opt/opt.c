/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "opt/opt.h"

#include "ir/verify.h"

static bool verify_after_pass(const PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
#ifdef PIR_VERIFY_ALL
    return pir_verify_module(mod, PIR_VERIFY_LIR, target, diag);
#else
    (void)mod; (void)target; (void)diag;
    return true;
#endif
}

typedef void (*FnPassSimple)(PirModule *, PirFunction *);
typedef void (*FnPassTarget)(PirModule *, PirFunction *, const PirTargetSpec *);

static bool run_simple(PirModule *mod, const PirTargetSpec *target, PirDiag *diag, FnPassSimple p) {
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        if (mod->funcs.data[fi].is_defined) {
            p(mod, &mod->funcs.data[fi]);
        }
    }
    return verify_after_pass(mod, target, diag);
}

static bool run_target(PirModule *mod, const PirTargetSpec *target, PirDiag *diag, FnPassTarget p) {
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        if (mod->funcs.data[fi].is_defined) {
            p(mod, &mod->funcs.data[fi], target);
        }
    }
    return verify_after_pass(mod, target, diag);
}

bool pir_opt_module(PirModule *mod, const PirTargetSpec *target, int opt_level, PirDiag *diag) {
    if (opt_level <= 0) {
        return true;
    }

    if (!run_simple(mod, target, diag, pir_opt_simplify_cfg)) return false;
    if (!run_simple(mod, target, diag, pir_opt_mem2reg))      return false;
    if (!run_target(mod, target, diag, pir_opt_gvn))          return false;

    if (opt_level >= 2) {
        if (!run_simple(mod, target, diag, pir_opt_dce))          return false;
        if (!run_simple(mod, target, diag, pir_opt_simplify_cfg)) return false;
        pir_opt_inline(mod, target);
        if (!verify_after_pass(mod, target, diag))                return false;
        if (!run_simple(mod, target, diag, pir_opt_simplify_cfg)) return false;
        if (!run_simple(mod, target, diag, pir_opt_mem2reg))      return false;
        if (!run_target(mod, target, diag, pir_opt_gvn))          return false;
        if (!run_target(mod, target, diag, pir_opt_licm))         return false;
        if (!run_target(mod, target, diag, pir_opt_dse))          return false;
        if (!run_target(mod, target, diag, pir_opt_gvn))          return false;
    }

    if (!run_simple(mod, target, diag, pir_opt_dce))          return false;
    if (!run_simple(mod, target, diag, pir_opt_simplify_cfg)) return false;
    if (opt_level >= 2) {
        if (!run_target(mod, target, diag, pir_opt_loop_vec))    return false;
        if (!run_simple(mod, target, diag, pir_opt_dce))         return false;
        if (!run_simple(mod, target, diag, pir_opt_loop_rotate)) return false;
        if (!run_target(mod, target, diag, pir_opt_licm))        return false;
        if (!run_target(mod, target, diag, pir_opt_dse))         return false;
        if (!run_simple(mod, target, diag, pir_opt_dce))         return false;
    }
    if (!run_simple(mod, target, diag, pir_opt_edge_split))   return false;

    return pir_verify_module(mod, PIR_VERIFY_LIR, target, diag);
}
