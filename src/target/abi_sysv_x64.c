/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "target/abi.h"

#include <string.h>

#include "lower/lower.h"

typedef enum {
    EB_NONE = 0,
    EB_SSE  = 1,
    EB_INT  = 2
} SysvClass;

typedef struct {
    SysvClass cls[2];
    uint32_t  f32_cnt[2];
    uint32_t  f64_cnt[2];
    bool      unaligned;
    bool      single_v128;
    PirType   v128_ty;
} SysvAggWalk;

static SysvClass merge_cls(SysvClass a, SysvClass b) {
    if (a == b) return a;
    if (a == EB_NONE) return b;
    if (b == EB_NONE) return a;
    if (a == EB_INT || b == EB_INT) return EB_INT;
    return EB_SSE;
}

static void walk_sysv(const PirModule *mod, const PirTargetSpec *target,
                      PirType ty, uint64_t base_off, SysvAggWalk *w) {
    if (ty.kind == PIR_TYPE_SCALAR) {
        PirTypeLayout ly = pir_type_layout(mod, target, ty);
        if (ly.align > 1u && (base_off % ly.align) != 0u) {
            w->unaligned = true;
            return;
        }
        uint64_t eb = base_off / 8u;
        if (ty.u.scalar == PIR_SCALAR_I128) {
            w->cls[0] = merge_cls(w->cls[0], EB_INT);
            w->cls[1] = merge_cls(w->cls[1], EB_INT);
        } else if (ty.u.scalar == PIR_SCALAR_F32) {
            if (eb < 2u) { w->cls[eb] = merge_cls(w->cls[eb], EB_SSE); w->f32_cnt[eb]++; }
        } else if (ty.u.scalar == PIR_SCALAR_F64) {
            if (eb < 2u) { w->cls[eb] = merge_cls(w->cls[eb], EB_SSE); w->f64_cnt[eb]++; }
        } else {
            if (eb < 2u) w->cls[eb] = merge_cls(w->cls[eb], EB_INT);
            uint64_t end_eb = (base_off + ly.size - 1u) / 8u;
            if (end_eb != eb && end_eb < 2u) w->cls[end_eb] = merge_cls(w->cls[end_eb], EB_INT);
        }
        return;
    }
    if (ty.kind == PIR_TYPE_VECTOR) {
        PirTypeLayout ly = pir_type_layout(mod, target, ty);
        if (ly.align > 1u && (base_off % ly.align) != 0u) {
            w->unaligned = true;
            return;
        }
        uint16_t bits = pir_vec_bits(ty.u.vec);
        if (bits == 128u && base_off == 0u) {
            w->single_v128 = true;
            w->v128_ty     = ty;
            w->cls[0] = merge_cls(w->cls[0], EB_SSE);
            w->cls[1] = merge_cls(w->cls[1], EB_SSE);
        } else {
            uint64_t eb = base_off / 8u;
            if (eb < 2u) w->cls[eb] = merge_cls(w->cls[eb], EB_SSE);
        }
        return;
    }
    const PirAggDef *agg = &mod->agg_types.data[ty.u.agg_id];
    if (agg->kind == PIR_AGG_PAD) {
        return; /* Explicit padding is NO_CLASS (EB_NONE) */
    }
    if (agg->kind == PIR_AGG_ARRAY) {
        PirTypeLayout el = pir_type_layout(mod, target, agg->elem_type);
        uint64_t stride = el.size;
        for (uint64_t i = 0u; i < agg->array_len; ++i) {
            walk_sysv(mod, target, agg->elem_type, base_off + i * stride, w);
        }
    } else {
        for (uint32_t i = 0u; i < agg->field_count; ++i) {
            uint64_t foff = pir_field_offset(mod, target, ty.u.agg_id, i);
            walk_sysv(mod, target, agg->fields[i], base_off + foff, w);
        }
    }
}

static void classify_sysv_agg(const PirModule *mod, const PirTargetSpec *target,
                              PirType agg_ty, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    PirTypeLayout ly = pir_type_layout(mod, target, agg_ty);
    if (ly.size == 0u || ly.size > 16u) {
        out->kind              = PIR_ABI_INDIRECT;
        out->gp_used           = 0u;
        out->stack_byval_size  = ly.size;
        out->stack_byval_align = ly.align;
        return;
    }
    SysvAggWalk w;
    memset(&w, 0, sizeof(w));
    walk_sysv(mod, target, agg_ty, 0u, &w);
    if (w.unaligned) {
        out->kind              = PIR_ABI_INDIRECT;
        out->gp_used           = 0u;
        out->stack_byval_size  = ly.size;
        out->stack_byval_align = ly.align;
        return;
    }
    if (w.single_v128 && ly.size == 16u) {
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = 1u;
        out->regs[0]   = w.v128_ty;
        out->fp_used   = 1u;
        return;
    }
    uint32_t neb = (ly.size <= 8u) ? 1u : 2u;
    out->kind = PIR_ABI_DIRECT;
    out->has_reg_offsets = true;
    for (uint32_t i = 0u; i < neb; ++i) {
        if (w.cls[i] == EB_NONE) {
            continue;
        }
        out->reg_offsets[out->reg_count] = (uint64_t)i * 8u;
        if (w.cls[i] == EB_SSE) {
            PirType st = pir_type_scalar(PIR_SCALAR_F64);
            if (w.f64_cnt[i] == 0u && w.f32_cnt[i] >= 1u && ((uint64_t)i * 8u + 4u >= ly.size)) {
                st = pir_type_scalar(PIR_SCALAR_F32);
            } else if (w.f64_cnt[i] == 0u && w.f32_cnt[i] >= 2u) {
                st = pir_type_vec(PIR_VEC_V2F32);
            }
            out->regs[out->reg_count++] = st;
            out->fp_used++;
        } else {
            out->regs[out->reg_count++] = pir_type_scalar(PIR_SCALAR_I64);
            out->gp_used++;
        }
    }
}

bool pir_abi_sysv_x64_param(const PirModule *mod, const PirTargetSpec *target,
                            const PirParamDecl *pd, PirAbiRegState *st, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    if (pd->abi_kind == PIR_PARAM_BYVAL) {
        classify_sysv_agg(mod, target, pd->abi_type, out);
        PirTypeLayout ly = pir_type_layout(mod, target, pd->abi_type);
        if (out->kind == PIR_ABI_DIRECT) {
            /* All-or-nothing register availability rule (SysV AMD64 §3.2.3). */
            if (st->gp_used + out->gp_used > 6u || st->fp_used + out->fp_used > 8u) {
                memset(out, 0, sizeof(*out));
                out->kind              = PIR_ABI_INDIRECT;
                out->stack_byval_size  = ly.size;
                out->stack_byval_align = ly.align;
            } else {
                st->gp_used += out->gp_used;
                st->fp_used += out->fp_used;
            }
        }
        return true;
    }
    /* Normal scalar/vector/i128 parameter: track GP/FP register consumption. */
    out->kind      = PIR_ABI_DIRECT;
    out->reg_count = 1u;
    out->regs[0]   = pd->type;
    if (pd->type.kind == PIR_TYPE_SCALAR && pd->type.u.scalar == PIR_SCALAR_I128) {
        if (st->gp_used + 2u <= 6u) {
            out->gp_used = 2u;
            st->gp_used += 2u;
        } else {
            /* SysV AMD64: i128 does not split across r9 and stack; 16-byte aligned on stack. */
            out->gp_used     = 0u;
            out->stack_align = 16u;
            out->force_stack = true;
        }
    } else if (pir_type_is_float(pd->type) || pd->type.kind == PIR_TYPE_VECTOR) {
        out->fp_used = 1u;
        if (st->fp_used < 8u) st->fp_used++;
    } else {
        out->gp_used = 1u;
        if (st->gp_used < 6u) st->gp_used++;
    }
    return true;
}

bool pir_abi_sysv_x64_retbuf(const PirModule *mod, const PirTargetSpec *target,
                             PirType agg_ty, PirAbiArgInfo *out) {
    classify_sysv_agg(mod, target, agg_ty, out);
    if (out->kind == PIR_ABI_INDIRECT) {
        /* Hidden sret pointer consumes rdi (1 GP reg) on SysV AMD64. */
        out->gp_used = 1u;
        out->fp_used = 0u;
    }
    return true;
}
