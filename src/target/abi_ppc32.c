/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * PowerPC 32-bit Big-Endian (SysV SVR4 ABI) C ABI Classifier.
 *
 * Rules (SVR4 PowerPC 32-bit + -msvr4-struct-return):
 *   - 8 GP argument registers: r3..r10 (4 bytes each).
 *   - 8 FP argument registers: f1..f8 (8 bytes each, single & double).
 *   - ALL aggregate parameters (byval(@T)) are passed INDIRECTLY via a
 *     pointer to a caller-allocated copy (consumes 1 GP reg r3..r10 or 4B stack).
 *   - 64-bit integer (i64) parameters require odd-register alignment
 *     (r3:r4, r5:r6, r7:r8, r9:r10 -> even 0-based GP index) and 8B stack alignment.
 *     If gp_used == 7 (only r10 left), i64 skips r10 and goes on 8B-aligned stack.
 *   - 128-bit integer (i128) parameters pass indirectly via pointer (or stack).
 *   - Aggregate return (retbuf(@T)):
 *       * Empty (size == 0): DIRECT 0 regs.
 *       * size <= 4B: DIRECT 1 GP reg (r3): i8 (1B), i16 (2B), i32 (3..4B).
 *       * 5..8B:      DIRECT 2 GP regs (r3:r4): { i32, i32 }.
 *       * > 8B:       INDIRECT hidden sret pointer in r3 (normal param 0).
 */

#include "target/abi.h"
#include "lower/lower.h"

#include <string.h>

static void pack_ppc32_ret_words(uint64_t size, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    out->kind = PIR_ABI_DIRECT;
    if (size == 0u) {
        out->reg_count = 0u;
        return;
    }
    if (size == 1u) {
        out->reg_count = 1u;
        out->regs[0]   = pir_type_scalar(PIR_SCALAR_I8);
        out->gp_used   = 1u;
    } else if (size == 2u) {
        out->reg_count = 1u;
        out->regs[0]   = pir_type_scalar(PIR_SCALAR_I16);
        out->gp_used   = 1u;
    } else if (size <= 4u) {
        out->reg_count = 1u;
        out->regs[0]   = pir_type_scalar(PIR_SCALAR_I32);
        out->gp_used   = 1u;
    } else {
        out->reg_count = 2u;
        out->regs[0]   = pir_type_scalar(PIR_SCALAR_I32);
        out->regs[1]   = pir_type_scalar(PIR_SCALAR_I32);
        out->gp_used   = 2u;
    }
}

bool pir_abi_ppc32_param(const PirModule *mod, const PirTargetSpec *target,
                         const PirParamDecl *pd, PirAbiRegState *st, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));

    if (pd->abi_kind == PIR_PARAM_BYVAL) {
        /* SysV PPC32 SVR4: ALL aggregate arguments pass indirectly by pointer. */
        out->kind    = PIR_ABI_INDIRECT;
        out->gp_used = 1u;
        if (st->gp_used < 8u) st->gp_used++;
        return true;
    }

    PirType ty = pd->type;
    if (ty.kind == PIR_TYPE_SCALAR) {
        switch (ty.u.scalar) {
            case PIR_SCALAR_F32:
            case PIR_SCALAR_F64:
                out->kind      = PIR_ABI_DIRECT;
                out->reg_count = 1u;
                out->regs[0]   = ty;
                if (st->fp_used < 8u) {
                    out->fp_used = 1u;
                    st->fp_used++;
                } else if (ty.u.scalar == PIR_SCALAR_F64) {
                    out->stack_align = 8u;
                }
                return true;

            case PIR_SCALAR_I64: {
                /* SVR4 PPC32: i64 requires even 0-based GP index (r3:r4, r5:r6, r7:r8, r9:r10)
                 * or 8B-aligned stack slot; never splits across r10 + stack. */
                out->kind        = PIR_ABI_DIRECT;
                out->reg_count   = 1u;
                out->regs[0]     = ty;
                out->stack_align = 8u;
                if ((st->gp_used & 1u) != 0u && st->gp_used < 8u) {
                    st->gp_used++;
                }
                if (st->gp_used + 2u <= 8u) {
                    out->gp_used = 2u;
                    st->gp_used += 2u;
                } else {
                    st->gp_used = 8u;
                    out->force_stack = true;
                }
                return true;
            }

            case PIR_SCALAR_I128:
                /* 16B integer passed indirectly via pointer on 32-bit PPC32. */
                out->kind    = PIR_ABI_INDIRECT;
                out->gp_used = 1u;
                if (st->gp_used < 8u) st->gp_used++;
                return true;

            case PIR_SCALAR_I8:
            case PIR_SCALAR_I16:
            case PIR_SCALAR_I32:
            case PIR_SCALAR_PTR:
            case PIR_SCALAR_VALIST:
            case PIR_SCALAR_VOID:
                out->kind      = PIR_ABI_DIRECT;
                out->reg_count = 1u;
                out->regs[0]   = ty;
                if (st->gp_used < 8u) {
                    out->gp_used = 1u;
                    st->gp_used++;
                }
                return true;
        }
    }

    /* Vectors (max_simd_bits == 0 on base PPC32 -> scalarized). */
    PirTypeLayout ly = pir_type_layout(mod, target, ty);
    if (ly.size > 8u) {
        out->kind      = PIR_ABI_INDIRECT;
        out->reg_count = 0u;
        out->gp_used   = 1u;
        if (st->gp_used < 8u) st->gp_used++;
        return true;
    }
    out->kind      = PIR_ABI_DIRECT;
    out->reg_count = 1u;
    out->regs[0]   = ty;
    uint32_t vgp = 0u, vfp = 0u;
    pir_abi_vec_param_regs(target, ty.u.vec, &vgp, &vfp);
    out->gp_used = vgp;
    out->fp_used = vfp;
    st->gp_used += vgp;
    if (st->gp_used > 8u) st->gp_used = 8u;
    st->fp_used += vfp;
    if (st->fp_used > 8u) st->fp_used = 8u;
    return true;
}

bool pir_abi_ppc32_retbuf(const PirModule *mod, const PirTargetSpec *target,
                          PirType agg_ty, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    PirTypeLayout ly = pir_type_layout(mod, target, agg_ty);
    if (ly.size == 0u) {
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = 0u;
        return true;
    }
    if (ly.size <= 8u) {
        pack_ppc32_ret_words(ly.size, out);
        return true;
    }
    /* > 8 bytes: hidden sret pointer in r3 (normal param 0). */
    out->kind    = PIR_ABI_INDIRECT;
    out->gp_used = 1u;
    return true;
}
