/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "target/abi.h"

#include <string.h>

#include "lower/lower.h"

typedef struct {
    bool     valid;
    bool     has_base;
    PirType  base_ty;
    uint32_t count;
} HfaWalk;

static bool is_hfa_leaf(const PirTargetSpec *target, PirType ty) {
    if (ty.kind == PIR_TYPE_SCALAR) {
        return ty.u.scalar == PIR_SCALAR_F32 || ty.u.scalar == PIR_SCALAR_F64;
    }
    if (ty.kind == PIR_TYPE_VECTOR) {
        uint16_t b = pir_vec_bits(ty.u.vec);
        return (b == 64u || b == 128u) && b <= target->caps.max_simd_bits;
    }
    return false;
}

static bool hfa_leaf_compat(PirType a, PirType b) {
    if (pir_type_eq(a, b)) return true;
    /* AAPCS64 HVA: any two short-vector types of equal total width (64b or 128b) are compatible. */
    if (a.kind == PIR_TYPE_VECTOR && b.kind == PIR_TYPE_VECTOR) {
        return pir_vec_bits(a.u.vec) == pir_vec_bits(b.u.vec);
    }
    return false;
}

static void walk_hfa(const PirModule *mod, const PirTargetSpec *target,
                     PirType ty, uint64_t base_off, HfaWalk *w) {
    if (!w->valid) return;
    if (ty.kind != PIR_TYPE_NAMED_AGG) {
        if (!is_hfa_leaf(target, ty)) { w->valid = false; return; }
        PirTypeLayout ly = pir_type_layout(mod, target, ty);
        if (ly.align > 1u && (base_off % ly.align) != 0u) { w->valid = false; return; }
        if (!w->has_base) {
            w->has_base = true;
            w->base_ty  = ty;
        } else if (!hfa_leaf_compat(w->base_ty, ty)) {
            w->valid = false;
            return;
        }
        w->count++;
        if (w->count > 4u) w->valid = false;
        return;
    }
    if (ty.u.agg_id >= mod->agg_types.len) {
        w->valid = false;
        return;
    }
    const PirAggDef *agg = &mod->agg_types.data[ty.u.agg_id];
    if (agg->kind == PIR_AGG_PAD) {
        w->valid = false;
        return;
    }
    if (agg->kind == PIR_AGG_ARRAY) {
        PirTypeLayout el = pir_type_layout(mod, target, agg->elem_type);
        if (el.size == 0u || agg->array_len == 0u) { w->valid = false; return; }
        for (uint64_t i = 0u; i < agg->array_len && w->valid; ++i) {
            walk_hfa(mod, target, agg->elem_type, base_off + i * el.size, w);
        }
    } else if (agg->kind == PIR_AGG_UNION) {
        if (agg->field_count == 0u) {
            w->valid = false;
            return;
        }
        uint32_t max_cnt = 0u;
        bool has_any = false;
        for (uint32_t i = 0u; i < agg->field_count && w->valid; ++i) {
            if (pir_abi_is_pad_only_agg(mod, agg->fields[i])) continue;
            has_any = true;
            HfaWalk mw = { true, false, pir_type_scalar(PIR_SCALAR_VOID), 0u };
            walk_hfa(mod, target, agg->fields[i], base_off, &mw);
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
            } else if (!hfa_leaf_compat(w->base_ty, mw.base_ty)) {
                w->valid = false;
                return;
            }
            if (mw.count > max_cnt) {
                max_cnt = mw.count;
            }
        }
        if (!has_any) {
            w->valid = false;
            return;
        }
        if (w->valid) {
            w->count += max_cnt;
            if (w->count > 4u) {
                w->valid = false;
            }
        }
    } else {
        for (uint32_t i = 0u; i < agg->field_count && w->valid; ++i) {
            uint64_t foff = pir_field_offset(mod, target, ty.u.agg_id, i);
            walk_hfa(mod, target, agg->fields[i], base_off + foff, w);
        }
    }
}

static void classify_aapcs64_agg(const PirModule *mod, const PirTargetSpec *target,
                                 PirType agg_ty, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    PirTypeLayout ly = pir_type_layout(mod, target, agg_ty);
    if (ly.size == 0u || pir_abi_is_pad_only_agg(mod, agg_ty)) {
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = 0u;
        out->gp_used   = 0u;
        out->fp_used   = 0u;
        return;
    }

    HfaWalk hw = { true, false, pir_type_scalar(PIR_SCALAR_VOID), 0u };
    walk_hfa(mod, target, agg_ty, 0u, &hw);
    if (hw.valid && hw.has_base && hw.count >= 1u && hw.count <= 4u) {
        PirTypeLayout bl = pir_type_layout(mod, target, hw.base_ty);
        if (ly.size == bl.size * hw.count) {
            out->kind      = PIR_ABI_DIRECT;
            out->reg_count = hw.count;
            out->fp_used   = hw.count;
            for (uint32_t i = 0u; i < hw.count; ++i) {
                out->regs[i] = hw.base_ty;
            }
            return;
        }
    }

    if (ly.size <= 16u) {
        uint32_t nregs = (ly.size <= 8u) ? 1u : 2u;
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = nregs;
        out->gp_used   = nregs;
        for (uint32_t i = 0u; i < nregs; ++i) {
            out->regs[i] = pir_type_scalar(PIR_SCALAR_I64);
        }
        return;
    }

    out->kind    = PIR_ABI_INDIRECT;
    out->gp_used = 1u;
}

bool pir_abi_aapcs64_param(const PirModule *mod, const PirTargetSpec *target,
                           const PirParamDecl *pd, PirAbiRegState *st, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    if (pd->abi_kind == PIR_PARAM_BYVAL) {
        classify_aapcs64_agg(mod, target, pd->abi_type, out);
        PirTypeLayout agg_ly = pir_type_layout(mod, target, pd->abi_type);
        uint32_t nat_al  = pir_abi_natural_align(mod, target, pd->abi_type);
        uint32_t nsaa_al = (nat_al > 16u) ? 16u : ((nat_al > 8u) ? nat_al : 8u);
        uint32_t dar_al  = (agg_ly.align > 16u) ? 16u : ((agg_ly.align > 8u) ? agg_ly.align : 8u);
        if (out->kind == PIR_ABI_DIRECT) {
            if (out->fp_used > 0u) {
                if (st->fp_used + out->fp_used > 8u) {
                    /* AAPCS64 all-or-nothing spill: consume remaining FP regs and pass by value on stack.
                     * On Darwin ARM64, fixed HFA stack alignment is the HFA base element alignment
                     * (4 for float, 8 for double), ignoring user _Alignas overrides.
                     * On Linux AAPCS64, stack slots are 8B minimum (capped at 16B natural align). */
                    PirTypeLayout bl = pir_type_layout(mod, target, out->regs[0]);
                    out->pad_fp_before = (st->fp_used < 8u) ? (8u - st->fp_used) : 0u;
                    st->fp_used = 8u;
                    out->stack_align = (target->os == PIR_OS_MACOS) ? bl.align : dar_al;
                    if (target->os != PIR_OS_MACOS &&
                        out->regs[0].kind == PIR_TYPE_SCALAR &&
                        out->regs[0].u.scalar == PIR_SCALAR_F32 &&
                        out->reg_count >= 2u) {
                        uint32_t n = out->reg_count;
                        uint32_t k = 0u;
                        while (n >= 2u) {
                            out->regs[k++] = pir_type_scalar(PIR_SCALAR_F64);
                            n -= 2u;
                        }
                        if (n == 1u) {
                            out->regs[k++] = pir_type_scalar(PIR_SCALAR_F32);
                        }
                        out->reg_count = k;
                        out->fp_used   = k;
                    }
                } else {
                    st->fp_used += out->fp_used;
                }
            } else {
                /* Linux AAPCS64 §6.8.2: 16B naturally-aligned composite in GP regs starts at even reg. */
                if (target->os != PIR_OS_MACOS && nat_al >= 16u &&
                    (st->gp_used & 1u) != 0u && st->gp_used < 8u) {
                    out->pad_gp_before++;
                    st->gp_used++;
                }
                if (st->gp_used + out->gp_used > 8u) {
                    /* AAPCS64 all-or-nothing spill: consume remaining GP regs and pass by value on stack.
                     * Darwin ARM64 aligns spilled non-HFA composites to effective align (clamped [8, 16]). */
                    if (st->gp_used < 8u) {
                        out->pad_gp_before += 8u - st->gp_used;
                    }
                    st->gp_used = 8u;
                    out->stack_align = (target->os == PIR_OS_MACOS) ? dar_al : nsaa_al;
                } else {
                    st->gp_used += out->gp_used;
                }
            }
        } else {
            if (st->gp_used < 8u) st->gp_used++;
        }
        return true;
    }

    out->kind      = PIR_ABI_DIRECT;
    out->reg_count = 1u;
    out->regs[0]   = pd->type;
    if (pd->type.kind == PIR_TYPE_SCALAR && pd->type.u.scalar == PIR_SCALAR_I128) {
        /* Linux AAPCS64 requires even GP register pairing (x0:x1, x2:x3, ...);
         * Darwin ARM64 does NOT insert odd-register padding. */
        if (target->os != PIR_OS_MACOS && (st->gp_used & 1u) != 0u && st->gp_used < 8u) {
            out->pad_gp_before++;
            st->gp_used++;
        }
        out->gp_used = 2u;
        if (st->gp_used + 2u <= 8u) {
            st->gp_used += 2u;
        } else {
            if (st->gp_used < 8u) {
                out->pad_gp_before += 8u - st->gp_used;
                st->gp_used = 8u;
            }
            out->stack_align = 16u;
        }
    } else if (pd->type.kind == PIR_TYPE_VECTOR) {
        uint32_t vgp = 0u, vfp = 0u;
        pir_abi_vec_param_regs(target, pd->type.u.vec, &vgp, &vfp);
        out->gp_used = vgp;
        out->fp_used = vfp;
        if (vgp > 0u) st->gp_used = (st->gp_used + vgp < 8u) ? (st->gp_used + vgp) : 8u;
        if (vfp > 0u) st->fp_used = (st->fp_used + vfp < 8u) ? (st->fp_used + vfp) : 8u;
    } else if (pir_type_is_float(pd->type)) {
        out->fp_used = 1u;
        if (st->fp_used < 8u) st->fp_used++;
    } else {
        out->gp_used = 1u;
        if (st->gp_used < 8u) st->gp_used++;
    }
    return true;
}

bool pir_abi_aapcs64_retbuf(const PirModule *mod, const PirTargetSpec *target,
                            PirType agg_ty, PirAbiArgInfo *out) {
    classify_aapcs64_agg(mod, target, agg_ty, out);
    if (out->kind == PIR_ABI_INDIRECT) {
        out->gp_used = 0u; /* ARM64 sret uses x8, not x0..x7 */
        out->fp_used = 0u;
    }
    return true;
}
