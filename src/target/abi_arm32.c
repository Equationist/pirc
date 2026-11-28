/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * ARM 32-bit AAPCS32 / AAPCS32-VFP (Thumb-2 Cortex-M7/M4/M3) C ABI Classifier.
 *
 * Official ARM AAPCS32 + VFP Co-processor Register Candidates:
 *   - Core GP argument registers: r0..r3 (4 x 32-bit).
 *   - VFP argument registers (hard-float, non-variadic): s0..s15 (16 x 32-bit,
 *     overlaid with d0..d7), with AAPCS-VFP backfilling tracked via a 16-bit
 *     occupancy mask stored in st->fp_used (bits 0..15 = s0..s15, bit 16 = spilled).
 *   - 8-byte aligned core arguments (i64, f64 in soft/variadic, align>=8 structs):
 *     even-register aligned (r0:r1 or r2:r3); if r3 would be split, skip r3 and
 *     spill to an 8-byte aligned stack slot.
 *   - HFA (Homogeneous Floating-Point Aggregate, hard-float non-variadic):
 *     1..4 f32 or 1..4 f64 elements -> passed in contiguous s/d regs (or stack),
 *     returned in s0..s3 / d0..d3.
 *   - Non-HFA aggregates:
 *       * retbuf: <= 4 bytes returned in r0; > 4 bytes via hidden sret pointer in r0.
 *       * byval:  <= 16 bytes passed directly as 1..4 i32 words (splitting across
 *                 remaining r0..r3 and stack); > 16 bytes passed indirectly by pointer.
 *   - i128 / vectors (32-bit target without SIMD): passed/returned indirectly via ptr.
 */

#include "target/abi.h"

#include <stdlib.h>
#include <string.h>

#include "lower/lower.h"

#define ARM32_VFP_SPILLED_BIT (1u << 16u)

typedef struct {
    bool     valid;
    bool     has_base;
    PirType  base_ty;
    uint32_t count;
    uint32_t req_align;
} Arm32HfaWalk;

static uint64_t arm32_align_up(uint64_t v, uint32_t al) {
    if (al <= 1u) return v;
    uint64_t m = (uint64_t)al - 1u;
    return (v + m) & ~m;
}

static bool arm32_is_hfa_leaf(PirType ty) {
    return ty.kind == PIR_TYPE_SCALAR &&
           (ty.u.scalar == PIR_SCALAR_F32 || ty.u.scalar == PIR_SCALAR_F64);
}

static void arm32_walk_hfa(const PirModule *mod, const PirTargetSpec *target,
                           PirType ty, uint64_t base_off, Arm32HfaWalk *w) {
    if (!w->valid) return;
    if (ty.kind != PIR_TYPE_NAMED_AGG) {
        if (!arm32_is_hfa_leaf(ty)) { w->valid = false; return; }
        PirTypeLayout ly = pir_type_layout(mod, target, ty);
        if (ly.align > 1u && (base_off % ly.align) != 0u) { w->valid = false; return; }
        if (!w->has_base) {
            w->has_base = true;
            w->base_ty  = ty;
        } else if (!pir_type_eq(w->base_ty, ty)) {
            w->valid = false;
            return;
        }
        if (ly.align > w->req_align) w->req_align = ly.align;
        w->count++;
        if (w->count > 4u) w->valid = false;
        return;
    }

    uint32_t aid = ty.u.agg_id;
    if (aid >= mod->agg_types.len) { w->valid = false; return; }
    const PirAggDef *agg = &mod->agg_types.data[aid];
    if (agg->kind == PIR_AGG_PAD) {
        w->valid = false;
    } else if (agg->kind == PIR_AGG_ARRAY) {
        PirTypeLayout el = pir_type_layout(mod, target, agg->elem_type);
        if (el.size == 0u || agg->array_len == 0u || agg->array_len > 4u) {
            w->valid = false;
        } else {
            for (uint64_t i = 0u; i < agg->array_len && w->valid; ++i) {
                arm32_walk_hfa(mod, target, agg->elem_type, base_off + i * el.size, w);
            }
        }
    } else if (agg->kind == PIR_AGG_UNION) {
        if (agg->field_count == 0u) { w->valid = false; return; }
        uint32_t max_cnt = 0u;
        bool has_any = false;
        for (uint32_t i = 0u; i < agg->field_count && w->valid; ++i) {
            if (pir_abi_is_pad_only_agg(mod, agg->fields[i])) continue;
            has_any = true;
            Arm32HfaWalk mw = { true, false, pir_type_scalar(PIR_SCALAR_VOID), 0u, 1u };
            arm32_walk_hfa(mod, target, agg->fields[i], 0u, &mw);
            if (!mw.valid || !mw.has_base || mw.count < 1u || mw.count > 4u) {
                w->valid = false;
                return;
            }
            PirTypeLayout ml = pir_type_layout(mod, target, agg->fields[i]);
            PirTypeLayout bl = pir_type_layout(mod, target, mw.base_ty);
            if (ml.size != bl.size * (uint64_t)mw.count) {
                w->valid = false;
                return;
            }
            if (!w->has_base) {
                w->has_base = true;
                w->base_ty  = mw.base_ty;
            } else if (!pir_type_eq(w->base_ty, mw.base_ty)) {
                w->valid = false;
                return;
            }
            if (mw.req_align > w->req_align) w->req_align = mw.req_align;
            if (mw.count > max_cnt) max_cnt = mw.count;
        }
        if (!has_any || w->count + max_cnt > 4u) w->valid = false;
        else w->count += max_cnt;
    } else {
        bool is_packed = (agg->kind == PIR_AGG_PACKED_STRUCT);
        uint64_t pos = 0u;
        for (uint32_t i = 0u; i < agg->field_count && w->valid; ++i) {
            PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[i]);
            uint64_t foff = is_packed ? pos : arm32_align_up(pos, fl.align);
            if (!pir_abi_is_pad_only_agg(mod, agg->fields[i])) {
                arm32_walk_hfa(mod, target, agg->fields[i], base_off + foff, w);
            }
            pos = foff + fl.size;
        }
    }
}

static bool arm32_detect_hfa(const PirModule *mod, const PirTargetSpec *target,
                             PirType agg_ty, PirType *out_base, uint32_t *out_cnt) {
    if (!target->caps.has_hw_fp) return false;
    Arm32HfaWalk w = { true, false, pir_type_scalar(PIR_SCALAR_VOID), 0u, 1u };
    arm32_walk_hfa(mod, target, agg_ty, 0u, &w);
    if (!w.valid || !w.has_base || w.count < 1u || w.count > 4u) return false;
    PirTypeLayout total = pir_type_layout(mod, target, agg_ty);
    PirTypeLayout elem  = pir_type_layout(mod, target, w.base_ty);
    if (total.size != elem.size * (uint64_t)w.count) return false;
    *out_base = w.base_ty;
    *out_cnt  = w.count;
    return true;
}

/* Try to allocate `n_slots` contiguous 32-bit VFP s-registers aligned to `align_slots` (1 or 2)
 * using AAPCS32-VFP backfilling. Updates *fp_state on success; marks spilled on failure. */
static bool arm32_alloc_vfp_slots(uint32_t *fp_state, uint32_t n_slots, uint32_t align_slots) {
    if ((*fp_state & ARM32_VFP_SPILLED_BIT) != 0u) return false;
    uint32_t mask = *fp_state & 0xFFFFu;
    uint32_t need = (1u << n_slots) - 1u;
    for (uint32_t s = 0u; s + n_slots <= 16u; s += align_slots) {
        if ((mask & (need << s)) == 0u) {
            *fp_state = (*fp_state & ~0xFFFFu) | (mask | (need << s));
            return true;
        }
    }
    *fp_state |= ARM32_VFP_SPILLED_BIT;
    return false;
}

bool pir_abi_arm32_retbuf(const PirModule *mod, const PirTargetSpec *target,
                          PirType agg_ty, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    PirTypeLayout ly = pir_type_layout(mod, target, agg_ty);
    if (ly.size == 0u || pir_abi_is_pad_only_agg(mod, agg_ty)) {
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = 0u;
        return true;
    }

    PirType hfa_base = pir_type_scalar(PIR_SCALAR_VOID);
    uint32_t hfa_cnt = 0u;
    if (arm32_detect_hfa(mod, target, agg_ty, &hfa_base, &hfa_cnt)) {
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = hfa_cnt;
        out->fp_used   = hfa_cnt;
        for (uint32_t k = 0u; k < hfa_cnt; ++k) out->regs[k] = hfa_base;
        return true;
    }

    /* Official AAPCS32: composite <= 4 bytes returns in r0; > 4 bytes uses hidden sret ptr in r0. */
    if (ly.size <= 4u) {
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = 1u;
        out->gp_used   = 1u;
        out->regs[0]   = pir_type_scalar(PIR_SCALAR_I32);
        return true;
    }

    out->kind    = PIR_ABI_INDIRECT;
    out->gp_used = 1u;
    return true;
}

bool pir_abi_arm32_param(const PirModule *mod, const PirTargetSpec *target,
                         const PirParamDecl *pd, PirAbiRegState *st, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    PirType ti32 = pir_type_scalar(PIR_SCALAR_I32);

    if (pd->abi_kind == PIR_PARAM_BYVAL) {
        PirType agg_ty = pd->abi_type;
        PirTypeLayout ly = pir_type_layout(mod, target, agg_ty);
        if (ly.size == 0u || pir_abi_is_pad_only_agg(mod, agg_ty)) {
            out->kind      = PIR_ABI_DIRECT;
            out->reg_count = 0u;
            return true;
        }

        /* Hard-float HFA (1..4 f32 or 1..4 f64) */
        PirType hfa_base = pir_type_scalar(PIR_SCALAR_VOID);
        uint32_t hfa_cnt = 0u;
        if (arm32_detect_hfa(mod, target, agg_ty, &hfa_base, &hfa_cnt)) {
            uint32_t slots_per = (hfa_base.u.scalar == PIR_SCALAR_F64) ? 2u : 1u;
            bool in_regs = arm32_alloc_vfp_slots(&st->fp_used, hfa_cnt * slots_per, slots_per);
            out->kind        = PIR_ABI_DIRECT;
            out->reg_count   = hfa_cnt;
            out->fp_used     = in_regs ? hfa_cnt : 0u;
            out->force_stack = !in_regs;
            out->stack_align = (slots_per == 2u) ? 8u : 4u;
            for (uint32_t k = 0u; k < hfa_cnt; ++k) out->regs[k] = hfa_base;
            return true;
        }

        /* Non-HFA composite: <= 16 bytes passes as 1..4 i32 words (splitting r0..r3 + stack). */
        if (ly.size <= 16u) {
            uint32_t nw = (uint32_t)((ly.size + 3u) / 4u);
            uint32_t nat_al = pir_abi_natural_align(mod, target, agg_ty);
            if (nat_al >= 8u) {
                if (st->gp_used < 4u && (st->gp_used & 1u) != 0u) {
                    st->gp_used++;
                }
                out->stack_align = 8u;
            }
            out->kind      = PIR_ABI_DIRECT;
            out->reg_count = nw;
            for (uint32_t k = 0u; k < nw; ++k) out->regs[k] = ti32;
            uint32_t rem = (st->gp_used < 4u) ? (4u - st->gp_used) : 0u;
            uint32_t used = (nw < rem) ? nw : rem;
            out->gp_used = used;
            st->gp_used += used;
            return true;
        }

        /* > 16 bytes non-HFA composite: pass indirectly via caller-copy pointer in 1 GP reg. */
        out->kind = PIR_ABI_INDIRECT;
        if (st->gp_used < 4u) {
            out->gp_used = 1u;
            st->gp_used++;
        }
        return true;
    }

    /* Normal scalar / vector parameters */
    PirType ty = pd->type;
    if (ty.kind == PIR_TYPE_VECTOR) {
        out->kind = PIR_ABI_INDIRECT;
        if (st->gp_used < 4u) {
            out->gp_used = 1u;
            st->gp_used++;
        }
        return true;
    }

    if (ty.kind == PIR_TYPE_SCALAR) {
        switch (ty.u.scalar) {
            case PIR_SCALAR_I128:
                out->kind = PIR_ABI_INDIRECT;
                if (st->gp_used < 4u) {
                    out->gp_used = 1u;
                    st->gp_used++;
                }
                return true;

            case PIR_SCALAR_I64:
                if (st->gp_used < 4u && (st->gp_used & 1u) != 0u) {
                    st->gp_used++;
                }
                out->kind        = PIR_ABI_DIRECT;
                out->reg_count   = 1u;
                out->regs[0]     = ty;
                out->stack_align = 8u;
                if (st->gp_used + 2u <= 4u) {
                    out->gp_used = 2u;
                    st->gp_used += 2u;
                } else {
                    st->gp_used = 4u;
                }
                return true;

            case PIR_SCALAR_F32:
                out->kind      = PIR_ABI_DIRECT;
                out->reg_count = 1u;
                out->regs[0]   = ty;
                if (target->caps.has_hw_fp) {
                    if (arm32_alloc_vfp_slots(&st->fp_used, 1u, 1u)) {
                        out->fp_used = 1u;
                    }
                } else if (st->gp_used < 4u) {
                    out->gp_used = 1u;
                    st->gp_used++;
                }
                return true;

            case PIR_SCALAR_F64:
                out->kind        = PIR_ABI_DIRECT;
                out->reg_count   = 1u;
                out->regs[0]     = ty;
                out->stack_align = 8u;
                if (target->caps.has_hw_fp) {
                    if (arm32_alloc_vfp_slots(&st->fp_used, 2u, 2u)) {
                        out->fp_used = 1u;
                    }
                } else {
                    if (st->gp_used < 4u && (st->gp_used & 1u) != 0u) {
                        st->gp_used++;
                    }
                    if (st->gp_used + 2u <= 4u) {
                        out->gp_used = 2u;
                        st->gp_used += 2u;
                    } else {
                        st->gp_used = 4u;
                    }
                }
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
                if (st->gp_used < 4u) {
                    out->gp_used = 1u;
                    st->gp_used++;
                }
                return true;
        }
    }

    return false;
}
