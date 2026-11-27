/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "target/abi.h"

#include <stdlib.h>
#include <string.h>

#include "lower/lower.h"

typedef enum {
    EB_NONE  = 0,
    EB_SSEUP = 1,
    EB_SSE   = 2,
    EB_INT   = 3
} SysvClass;

typedef struct {
    SysvClass cls[2];
    uint32_t  f32_cnt[2];
    uint32_t  f64_cnt[2];
    bool      unaligned;
    bool      single_v128;
    PirType   v128_ty;
    bool      single_v256;
    bool      has_v256;
    bool      has_explicit_align32;
    PirType   v256_ty;
    uint32_t  non_v256_leaves;
} SysvAggWalk;

static SysvClass merge_cls(SysvClass a, SysvClass b) {
    if (a == b) return a;
    if (a == EB_NONE) return b;
    if (b == EB_NONE) return a;
    if (a == EB_INT || b == EB_INT) return EB_INT;
    if (a == EB_SSE || b == EB_SSE) return EB_SSE;
    return EB_SSEUP;
}

static uint64_t sysv_align_up(uint64_t v, uint32_t al) {
    if (al <= 1u) return v;
    uint64_t m = (uint64_t)al - 1u;
    return (v + m) & ~m;
}

static void walk_sysv(const PirModule *mod, const PirTargetSpec *target,
                      PirType ty, uint64_t base_off, SysvAggWalk *w,
                      uint16_t *seen_off) {
    if (w->unaligned) return;
    if (ty.kind == PIR_TYPE_SCALAR) {
        PirTypeLayout ly = pir_type_layout(mod, target, ty);
        if (ly.align > 1u && (base_off % ly.align) != 0u) {
            w->unaligned = true;
            return;
        }
        w->non_v256_leaves++;
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
            if (ly.size > 0u) {
                uint64_t end_eb = (base_off + ly.size - 1u) / 8u;
                if (end_eb != eb && end_eb < 2u) w->cls[end_eb] = merge_cls(w->cls[end_eb], EB_INT);
            }
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
        if (bits == 256u) {
            w->has_v256 = true;
            if (base_off == 0u) {
                w->single_v256 = true;
                w->v256_ty     = ty;
                w->cls[0] = merge_cls(w->cls[0], EB_SSE);
                w->cls[1] = merge_cls(w->cls[1], EB_SSEUP);
            } else {
                w->non_v256_leaves++;
            }
        } else if (bits == 128u && base_off == 0u) {
            w->non_v256_leaves++;
            w->single_v128 = true;
            w->v128_ty     = ty;
            w->cls[0] = merge_cls(w->cls[0], EB_SSE);
            w->cls[1] = merge_cls(w->cls[1], EB_SSEUP);
        } else {
            w->non_v256_leaves++;
            uint64_t eb = base_off / 8u;
            if (eb < 2u) w->cls[eb] = merge_cls(w->cls[eb], EB_SSE);
        }
        return;
    }
    uint32_t aid = ty.u.agg_id;
    if (aid >= mod->agg_types.len) {
        w->unaligned = true;
        return;
    }
    if (seen_off && base_off < 16u) {
        uint16_t bit = (uint16_t)(1u << base_off);
        if ((seen_off[aid] & bit) != 0u) return;
        seen_off[aid] |= bit;
    }
    const PirAggDef *agg = &mod->agg_types.data[aid];
    if (agg->min_align >= 32u) {
        w->has_explicit_align32 = true;
    }
    if (agg->abi_memory) {
        w->unaligned = true;
        return;
    }
    if (agg->kind == PIR_AGG_PAD) {
        return; /* Explicit padding is NO_CLASS (EB_NONE) */
    }
    PirTypeLayout aly = pir_type_layout(mod, target, ty);
    if (aly.align > 1u && (base_off % aly.align) != 0u) {
        w->unaligned = true;
        return;
    }
    if (agg->kind == PIR_AGG_ARRAY) {
        PirTypeLayout el = pir_type_layout(mod, target, agg->elem_type);
        if (el.size == 0u || agg->array_len == 0u) return;
        uint64_t stride = el.size;
        for (uint64_t i = 0u; i < agg->array_len && !w->unaligned; ++i) {
            walk_sysv(mod, target, agg->elem_type, base_off + i * stride, w, seen_off);
        }
    } else if (agg->kind == PIR_AGG_UNION) {
        for (uint32_t i = 0u; i < agg->field_count && !w->unaligned; ++i) {
            walk_sysv(mod, target, agg->fields[i], base_off, w, seen_off);
        }
    } else {
        uint64_t pos = 0u;
        for (uint32_t i = 0u; i < agg->field_count && !w->unaligned; ++i) {
            PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[i]);
            uint64_t foff = (agg->kind == PIR_AGG_STRUCT) ? sysv_align_up(pos, fl.align) : pos;
            walk_sysv(mod, target, agg->fields[i], base_off + foff, w, seen_off);
            pos = foff + fl.size;
        }
    }
}

static void run_walk_sysv(const PirModule *mod, const PirTargetSpec *target,
                          PirType agg_ty, SysvAggWalk *w) {
    uint32_t n = mod->agg_types.len;
    uint16_t stack_seen[64];
    uint16_t *seen = (n <= 64u) ? stack_seen : (uint16_t *)calloc(n, sizeof(uint16_t));
    if (!seen) abort();
    if (seen == stack_seen) memset(stack_seen, 0, n * sizeof(uint16_t));
    memset(w, 0, sizeof(*w));
    walk_sysv(mod, target, agg_ty, 0u, w, seen);
    if (seen != stack_seen) free(seen);
}

static void classify_sysv_agg(const PirModule *mod, const PirTargetSpec *target,
                              PirType agg_ty, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    PirTypeLayout ly = pir_type_layout(mod, target, agg_ty);
    if (ly.size == 0u) {
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = 0u;
        out->gp_used   = 0u;
        out->fp_used   = 0u;
        return;
    }
    if (ly.size > 16u) {
        if (ly.size == 32u && target->caps.max_simd_bits >= 256u) {
            SysvAggWalk w2;
            run_walk_sysv(mod, target, agg_ty, &w2);
            if (!w2.unaligned && w2.single_v256 && w2.non_v256_leaves == 0u) {
                out->kind      = PIR_ABI_DIRECT;
                out->reg_count = 1u;
                out->regs[0]   = w2.v256_ty;
                out->fp_used   = 1u;
                return;
            }
        }
        uint32_t byval_al = ly.align;
        if (target->os == PIR_OS_MACOS && target->caps.max_simd_bits < 256u &&
            byval_al == 32u && agg_ty.kind == PIR_TYPE_NAMED_AGG &&
            agg_ty.u.agg_id < mod->agg_types.len &&
            mod->agg_types.data[agg_ty.u.agg_id].min_align < 32u) {
            /* Apple x86_64 without AVX caps v256 aggregate stack alignment at 16B
             * unless explicitly over-aligned with _Alignas(>=32). */
            SysvAggWalk w2;
            run_walk_sysv(mod, target, agg_ty, &w2);
            if (w2.has_v256 && !w2.has_explicit_align32) byval_al = 16u;
        }
        out->kind              = PIR_ABI_INDIRECT;
        out->gp_used           = 0u;
        out->stack_byval_size  = ly.size;
        out->stack_byval_align = byval_al;
        return;
    }
    SysvAggWalk w;
    run_walk_sysv(mod, target, agg_ty, &w);
    if (w.unaligned) {
        out->kind              = PIR_ABI_INDIRECT;
        out->gp_used           = 0u;
        out->stack_byval_size  = ly.size;
        out->stack_byval_align = ly.align;
        return;
    }
    if (w.single_v128 && ly.size == 16u && w.cls[0] == EB_SSE && w.cls[1] == EB_SSEUP &&
        target->caps.max_simd_bits >= 128u) {
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
        if (w.cls[i] == EB_SSE || w.cls[i] == EB_SSEUP) {
            PirType st = pir_type_scalar(PIR_SCALAR_F64);
            if (w.f64_cnt[i] == 0u && w.f32_cnt[i] >= 1u && ((uint64_t)i * 8u + 4u >= ly.size)) {
                st = pir_type_scalar(PIR_SCALAR_F32);
            } else if (w.f64_cnt[i] == 0u && w.f32_cnt[i] >= 2u) {
                st = (target->caps.max_simd_bits >= 64u)
                     ? pir_type_vec(PIR_VEC_V2F32) : pir_type_scalar(PIR_SCALAR_F64);
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
            if ((out->gp_used > 0u && st->gp_used + out->gp_used > 6u) ||
                (out->fp_used > 0u && st->fp_used + out->fp_used > 8u)) {
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
    } else if (pd->type.kind == PIR_TYPE_VECTOR) {
        uint16_t vbits = pir_vec_bits(pd->type.u.vec);
        if (vbits > 128u && target->caps.max_simd_bits < 256u) {
            /* SysV AMD64 without AVX: 256-bit vectors are MEMORY class (inline 32B stack slot).
             * Apple x86_64 without AVX aligns v256 stack slots to 16B; Linux aligns to 32B. */
            PirTypeLayout vly = pir_type_layout(mod, target, pd->type);
            out->kind              = PIR_ABI_INDIRECT;
            out->reg_count         = 0u;
            out->gp_used           = 0u;
            out->fp_used           = 0u;
            out->stack_byval_size  = vly.size;
            out->stack_byval_align = (target->os == PIR_OS_MACOS && vly.align > 16u) ? 16u : vly.align;
            return true;
        }
        uint32_t vgp = 0u, vfp = 0u;
        pir_abi_vec_param_regs(target, pd->type.u.vec, &vgp, &vfp);
        out->gp_used = vgp;
        out->fp_used = vfp;
        if (vgp > 0u) st->gp_used = (st->gp_used + vgp < 6u) ? (st->gp_used + vgp) : 6u;
        if (vfp > 0u) st->fp_used = (st->fp_used + vfp < 8u) ? (st->fp_used + vfp) : 8u;
    } else if (pir_type_is_float(pd->type)) {
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
