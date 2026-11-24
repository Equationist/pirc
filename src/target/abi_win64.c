/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "target/abi.h"

#include <string.h>

#include "lower/lower.h"

static void classify_win64_agg(const PirModule *mod, const PirTargetSpec *target,
                               PirType agg_ty, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    PirTypeLayout ly = pir_type_layout(mod, target, agg_ty);
    if (ly.size == 1u || ly.size == 2u || ly.size == 4u || ly.size == 8u) {
        PirScalarType sc = (ly.size == 1u) ? PIR_SCALAR_I8
                         : (ly.size == 2u) ? PIR_SCALAR_I16
                         : (ly.size == 4u) ? PIR_SCALAR_I32 : PIR_SCALAR_I64;
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = 1u;
        out->regs[0]   = pir_type_scalar(sc);
        out->gp_used   = 1u;
        return;
    }
    out->kind    = PIR_ABI_INDIRECT;
    out->gp_used = 1u;
}

bool pir_abi_win64_param(const PirModule *mod, const PirTargetSpec *target,
                         const PirParamDecl *pd, PirAbiRegState *st, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    if (pd->abi_kind == PIR_PARAM_BYVAL) {
        classify_win64_agg(mod, target, pd->abi_type, out);
    } else {
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = 1u;
        out->regs[0]   = pd->type;
    }
    /* Win64 consumes 1 unified slot per argument (rcx/xmm0, rdx/xmm1, r8/xmm2, r9/xmm3). */
    if (st->gp_used < 4u) st->gp_used++;
    return true;
}

bool pir_abi_win64_retbuf(const PirModule *mod, const PirTargetSpec *target,
                          PirType agg_ty, PirAbiArgInfo *out) {
    classify_win64_agg(mod, target, agg_ty, out);
    return true;
}
