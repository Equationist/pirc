/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * RISC-V 32-bit ILP32D (RV32IMAFD) Calling Convention Classifier.
 *
 * Official RISC-V psABI (ILP32D, XLEN=32, FLEN=64):
 *   - GP argument registers: a0..a7 (8 x 32-bit)
 *   - FP argument registers: fa0..fa7 (8 x 64-bit)
 *   - Hardware-FP struct flattening (non-union, non-packed, up to 2 leaves):
 *       * 1 float (f32/f64)                 -> 1 FP reg
 *       * 2 floats (f32/f64 in any combo)   -> 2 FP regs
 *       * 1 float + 1 int (<= 4B)           -> 1 FP reg + 1 GP reg (preserving offsets)
 *     Only used if enough FP (and GP) registers remain; otherwise falls back
 *     to the Integer Calling Convention.
 *   - Integer Calling Convention fallback for aggregates:
 *       * size == 0 / pad-only -> 0 regs
 *       * size <= 8 bytes (2*XLEN) -> 1 (<=4B) or 2 (5..8B) i32 GP words
 *         (if 1 GP reg remains for an 8B struct, lo word in a7, hi word on stack!)
 *       * size > 8 bytes -> passed indirectly by pointer (caller copy) in 1 GP reg,
 *         or returned via hidden sret pointer in a0.
 *   - Scalars:
 *       * i8/i16/i32/ptr: 1 GP reg
 *       * i64: 2 GP regs (NO even-register alignment for non-variadic calls;
 *         may split lo in a7 and hi on stack!)
 *       * f32/f64: 1 FP reg if fp_used < 8; else 1 or 2 GP regs (or stack).
 *       * i128: passed/returned via 16B caller-copy / hidden sret pointer (> 2*XLEN).
 */

#include "target/abi.h"

#include <stdlib.h>
#include <string.h>

#include "lower/lower.h"

typedef struct {
    bool     valid;
    uint32_t count;
    PirType  tys[2];
    uint64_t offs[2];
    uint32_t fp_cnt;
    uint32_t gp_cnt;
} Rv32FpStructWalk;

static uint64_t rv32_align_up(uint64_t v, uint32_t al) {
    if (al <= 1u) return v;
    uint64_t m = (uint64_t)al - 1u;
    return (v + m) & ~m;
}

static void walk_rv32_fp_struct(const PirModule *mod, const PirTargetSpec *target,
                                PirType ty, uint64_t base_off, Rv32FpStructWalk *w) {
    if (!w->valid) return;
    if (ty.kind == PIR_TYPE_SCALAR) {
        if (ty.u.scalar == PIR_SCALAR_VOID) return;
        if (w->count >= 2u) {
            w->valid = false;
            return;
        }
        PirTypeLayout ly = pir_type_layout(mod, target, ty);
        if (ty.u.scalar == PIR_SCALAR_F32 || ty.u.scalar == PIR_SCALAR_F64) {
            w->tys[w->count]  = ty;
            w->offs[w->count] = base_off;
            w->count++;
            w->fp_cnt++;
            return;
        }
        if (ly.size <= 4u &&
            (ty.u.scalar == PIR_SCALAR_I8 || ty.u.scalar == PIR_SCALAR_I16 ||
             ty.u.scalar == PIR_SCALAR_I32 || ty.u.scalar == PIR_SCALAR_PTR)) {
            w->tys[w->count]  = ty;
            w->offs[w->count] = base_off;
            w->count++;
            w->gp_cnt++;
            if (w->gp_cnt > 1u) w->valid = false;
            return;
        }
        w->valid = false;
        return;
    }
    if (ty.kind == PIR_TYPE_VECTOR) {
        w->valid = false;
        return;
    }
    /* PIR_TYPE_NAMED_AGG */
    uint32_t aid = ty.u.agg_id;
    if (aid >= mod->agg_types.len) {
        w->valid = false;
        return;
    }
    const PirAggDef *agg = &mod->agg_types.data[aid];
    if (agg->kind == PIR_AGG_PAD) {
        return;
    }
    if (agg->kind == PIR_AGG_UNION) {
        w->valid = false;
        return;
    }
    if (agg->kind == PIR_AGG_ARRAY) {
        if (agg->array_len == 0u) return;
        PirTypeLayout el = pir_type_layout(mod, target, agg->elem_type);
        if (el.size == 0u || pir_abi_is_pad_only_agg(mod, agg->elem_type)) return;
        if (agg->array_len > 2u) {
            w->valid = false;
            return;
        }
        for (uint64_t i = 0u; i < agg->array_len && w->valid; ++i) {
            walk_rv32_fp_struct(mod, target, agg->elem_type, base_off + i * el.size, w);
        }
        return;
    }
    /* PIR_AGG_STRUCT or PIR_AGG_PACKED_STRUCT */
    bool is_packed = (agg->kind == PIR_AGG_PACKED_STRUCT);
    uint64_t pos = 0u;
    for (uint32_t i = 0u; i < agg->field_count && w->valid; ++i) {
        PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[i]);
        uint64_t foff = is_packed ? pos : rv32_align_up(pos, fl.align);
        walk_rv32_fp_struct(mod, target, agg->fields[i], base_off + foff, w);
        pos = foff + fl.size;
    }
}

static bool try_rv32_fp_struct(const PirModule *mod, const PirTargetSpec *target,
                               PirType agg_ty, uint32_t avail_gp, uint32_t avail_fp,
                               PirAbiArgInfo *out) {
    Rv32FpStructWalk w;
    memset(&w, 0, sizeof(w));
    w.valid = true;
    walk_rv32_fp_struct(mod, target, agg_ty, 0u, &w);
    if (!w.valid || w.fp_cnt == 0u || w.count == 0u || w.count > 2u) {
        return false;
    }
    if (w.fp_cnt > avail_fp || w.gp_cnt > avail_gp) {
        return false;
    }
    out->kind            = PIR_ABI_DIRECT;
    out->reg_count       = w.count;
    out->gp_used         = w.gp_cnt;
    out->fp_used         = w.fp_cnt;
    out->has_reg_offsets = true;
    for (uint32_t i = 0u; i < w.count; ++i) {
        out->regs[i]        = w.tys[i];
        out->reg_offsets[i] = w.offs[i];
    }
    return true;
}

bool pir_abi_rv32_param(const PirModule *mod, const PirTargetSpec *target,
                        const PirParamDecl *pd, PirAbiRegState *st, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    PirType ti32 = pir_type_scalar(PIR_SCALAR_I32);

    if (pd->abi_kind == PIR_PARAM_BYVAL) {
        PirTypeLayout ly = pir_type_layout(mod, target, pd->abi_type);
        if (ly.size == 0u || pir_abi_is_pad_only_agg(mod, pd->abi_type)) {
            out->kind      = PIR_ABI_DIRECT;
            out->reg_count = 0u;
            return true;
        }
        uint32_t avail_gp = (st->gp_used < 8u) ? (8u - st->gp_used) : 0u;
        uint32_t avail_fp = (st->fp_used < 8u) ? (8u - st->fp_used) : 0u;
        if (try_rv32_fp_struct(mod, target, pd->abi_type, avail_gp, avail_fp, out)) {
            st->gp_used += out->gp_used;
            st->fp_used += out->fp_used;
            return true;
        }
        /* Integer calling convention fallback: <= 8 bytes in 1..2 i32 words, else indirect ptr. */
        if (ly.size <= 8u) {
            uint32_t nregs = (ly.size <= 4u) ? 1u : 2u;
            out->kind      = PIR_ABI_DIRECT;
            out->reg_count = nregs;
            out->gp_used   = nregs;
            for (uint32_t i = 0u; i < nregs; ++i) {
                out->regs[i] = ti32;
            }
            if (ly.align >= 8u && st->gp_used >= 8u) {
                out->stack_align = 8u;
            }
            st->gp_used += nregs;
            if (st->gp_used > 8u) st->gp_used = 8u;
            return true;
        }
        out->kind    = PIR_ABI_INDIRECT;
        out->gp_used = 1u;
        if (st->gp_used < 8u) st->gp_used++;
        return true;
    }

    /* Normal scalar/vector parameter */
    if (pd->type.kind == PIR_TYPE_SCALAR && pd->type.u.scalar == PIR_SCALAR_I128) {
        /* 16B scalar (> 2*XLEN): passed indirectly by pointer on RV32. */
        out->kind    = PIR_ABI_INDIRECT;
        out->gp_used = 1u;
        if (st->gp_used < 8u) st->gp_used++;
        return true;
    }

    out->kind      = PIR_ABI_DIRECT;
    out->reg_count = 1u;
    out->regs[0]   = pd->type;

    if (pd->type.kind == PIR_TYPE_SCALAR) {
        switch (pd->type.u.scalar) {
            case PIR_SCALAR_F32:
                if (st->fp_used < 8u) {
                    out->fp_used = 1u;
                    st->fp_used++;
                } else {
                    out->gp_used = 1u;
                    if (st->gp_used < 8u) st->gp_used++;
                }
                break;
            case PIR_SCALAR_F64:
                if (st->fp_used < 8u) {
                    out->fp_used = 1u;
                    st->fp_used++;
                } else {
                    out->gp_used = 2u;
                    if (st->gp_used >= 8u) out->stack_align = 8u;
                    st->gp_used += 2u;
                    if (st->gp_used > 8u) st->gp_used = 8u;
                }
                break;
            case PIR_SCALAR_I64:
                out->gp_used = 2u;
                if (st->gp_used >= 8u) out->stack_align = 8u;
                st->gp_used += 2u;
                if (st->gp_used > 8u) st->gp_used = 8u;
                break;
            case PIR_SCALAR_I8:
            case PIR_SCALAR_I16:
            case PIR_SCALAR_I32:
            case PIR_SCALAR_PTR:
            case PIR_SCALAR_VOID:
            case PIR_SCALAR_I128:
            case PIR_SCALAR_VALIST:
                out->gp_used = 1u;
                if (st->gp_used < 8u) st->gp_used++;
                break;
        }
        return true;
    }

    if (pd->type.kind == PIR_TYPE_VECTOR) {
        /* On RV32 (max_simd_bits == 0), vectors > 8B are passed indirectly; <= 8B scalarized. */
        PirTypeLayout ly = pir_type_layout(mod, target, pd->type);
        if (ly.size > 8u) {
            out->kind      = PIR_ABI_INDIRECT;
            out->reg_count = 0u;
            out->gp_used   = 1u;
            if (st->gp_used < 8u) st->gp_used++;
            return true;
        }
        uint32_t gp = 0u, fp = 0u;
        pir_abi_vec_param_regs(target, pd->type.u.vec, &gp, &fp);
        out->gp_used = gp;
        out->fp_used = fp;
        st->gp_used += gp;
        if (st->gp_used > 8u) st->gp_used = 8u;
        st->fp_used += fp;
        if (st->fp_used > 8u) st->fp_used = 8u;
        return true;
    }

    return true;
}

bool pir_abi_rv32_retbuf(const PirModule *mod, const PirTargetSpec *target,
                         PirType agg_ty, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    PirTypeLayout ly = pir_type_layout(mod, target, agg_ty);
    if (ly.size == 0u || pir_abi_is_pad_only_agg(mod, agg_ty)) {
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = 0u;
        return true;
    }
    /* Return registers on ILP32D: up to 2 GP (a0, a1) and/or 2 FP (fa0, fa1). */
    if (try_rv32_fp_struct(mod, target, agg_ty, 2u, 2u, out)) {
        return true;
    }
    if (ly.size <= 8u) {
        uint32_t nregs = (ly.size <= 4u) ? 1u : 2u;
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = nregs;
        out->gp_used   = nregs;
        for (uint32_t i = 0u; i < nregs; ++i) {
            out->regs[i] = pir_type_scalar(PIR_SCALAR_I32);
        }
        return true;
    }
    /* > 8B: hidden sret pointer passed as first GP argument in a0 (consumes 1 GP arg reg). */
    out->kind    = PIR_ABI_INDIRECT;
    out->gp_used = 1u;
    return true;
}
