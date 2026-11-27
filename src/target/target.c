/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "target/target.h"
#include "target/abi.h"
#include "lower/lower.h"

#include <stdlib.h>
#include <string.h>

bool pir_target_from_triple(const char *triple, PirTargetSpec *out) {
    if (!triple || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->triple = triple;
    out->caps.has_mul_wide  = true;
    out->caps.has_add_carry = true;
    out->caps.has_ovf_ops   = true;
    out->caps.has_bitops    = true;
    out->caps.has_switch    = true;

    if (strcmp(triple, "x86_64-unknown-linux-gnu") == 0 ||
        strcmp(triple, "x86_64-linux-gnu") == 0 ||
        strcmp(triple, "x86_64-apple-darwin") == 0) {
        out->arch           = PIR_ARCH_X86_64;
        out->os             = strstr(triple, "darwin") ? PIR_OS_MACOS : PIR_OS_LINUX;
        out->ptr_bytes      = 8u;
        out->is_big_endian  = false;
        out->va_list_size   = 24u;
        out->va_list_align  = 8u;
        out->caps.max_simd_bits = 128u;
        return true;
    }
    if (strcmp(triple, "x86_64-pc-windows-msvc") == 0) {
        out->arch           = PIR_ARCH_X86_64;
        out->os             = PIR_OS_WINDOWS;
        out->ptr_bytes      = 8u;
        out->is_big_endian  = false;
        out->va_list_size   = 8u;
        out->va_list_align  = 8u;
        out->caps.max_simd_bits = 128u;
        return true;
    }
    if (strcmp(triple, "aarch64-unknown-linux-gnu") == 0 ||
        strcmp(triple, "aarch64-linux-gnu") == 0) {
        out->arch           = PIR_ARCH_ARM64;
        out->os             = PIR_OS_LINUX;
        out->ptr_bytes      = 8u;
        out->is_big_endian  = false;
        out->va_list_size   = 32u;
        out->va_list_align  = 8u;
        out->caps.max_simd_bits = 128u;
        return true;
    }
    if (strcmp(triple, "aarch64-apple-darwin") == 0 ||
        strcmp(triple, "arm64-apple-darwin") == 0) {
        out->arch           = PIR_ARCH_ARM64;
        out->os             = PIR_OS_MACOS;
        out->ptr_bytes      = 8u;
        out->is_big_endian  = false;
        out->va_list_size   = 8u;
        out->va_list_align  = 8u;
        out->caps.max_simd_bits = 128u;
        return true;
    }
    if (strcmp(triple, "wasm32-unknown-unknown") == 0 ||
        strcmp(triple, "i386-unknown-linux-gnu") == 0 ||
        strcmp(triple, "i686-unknown-linux-gnu") == 0 ||
        strcmp(triple, "riscv32-unknown-elf") == 0) {
        out->arch           = PIR_ARCH_CUSTOM;
        out->os             = PIR_OS_BAREMETAL;
        out->ptr_bytes      = 4u;
        out->is_big_endian  = false;
        out->va_list_size   = 4u;
        out->va_list_align  = 4u;
        out->caps.max_simd_bits = 128u;
        return true;
    }
    if (strcmp(triple, "riscv64-unknown-linux-gnu") == 0) {
        out->arch           = PIR_ARCH_CUSTOM;
        out->os             = PIR_OS_LINUX;
        out->ptr_bytes      = 8u;
        out->is_big_endian  = false;
        out->va_list_size   = 8u;
        out->va_list_align  = 8u;
        out->caps.max_simd_bits = 0u;
        return true;
    }
    return false;
}

bool pir_target_apply_caps(PirTargetSpec *spec, const char *profile) {
    if (!spec || !profile) {
        return false;
    }
    if (strcmp(profile, "default") == 0) {
        return true;
    }
    if (strcmp(profile, "scalar") == 0) {
        spec->caps.max_simd_bits = 0u;
        return true;
    }
    if (strcmp(profile, "avx2") == 0) {
        spec->caps.max_simd_bits = 256u;
        return true;
    }
    if (strcmp(profile, "minimal") == 0) {
        spec->caps.max_simd_bits = 0u;
        spec->caps.has_mul_wide  = false;
        spec->caps.has_add_carry = false;
        spec->caps.has_ovf_ops   = false;
        spec->caps.has_bitops    = false;
        spec->caps.has_switch    = false;
        return true;
    }
    return false;
}

bool pir_abi_is_pad_only_agg(const PirModule *mod, PirType ty) {
    return pir_agg_is_pad_only(mod, ty);
}

static uint32_t pir_abi_natural_align_rec(const PirModule *mod, const PirTargetSpec *target,
                                          PirType ty, bool is_outer, uint32_t *cache) {
    if (ty.kind != PIR_TYPE_NAMED_AGG || ty.u.agg_id >= mod->agg_types.len) {
        return pir_type_layout(mod, target, ty).align;
    }
    uint32_t aid = ty.u.agg_id;
    if (!is_outer && cache && cache[aid] != 0u) {
        return cache[aid];
    }
    const PirAggDef *agg = &mod->agg_types.data[aid];
    uint32_t res = 1u;
    if (agg->kind == PIR_AGG_PACKED_STRUCT || agg->kind == PIR_AGG_PAD) {
        res = (!is_outer && agg->min_align > 1u) ? agg->min_align : 1u;
    } else if (agg->kind == PIR_AGG_ARRAY) {
        uint32_t ea = pir_abi_natural_align_rec(mod, target, agg->elem_type, false, cache);
        if (!is_outer && agg->min_align > ea) ea = agg->min_align;
        res = ea;
    } else {
        uint32_t max_al = (!is_outer && agg->min_align > 1u) ? agg->min_align : 1u;
        for (uint32_t i = 0u; i < agg->field_count; ++i) {
            uint32_t fa = pir_abi_natural_align_rec(mod, target, agg->fields[i], false, cache);
            if (fa > max_al) max_al = fa;
        }
        res = max_al;
    }
    if (!is_outer && cache) cache[aid] = res;
    return res;
}

uint32_t pir_abi_natural_align(const PirModule *mod, const PirTargetSpec *target, PirType ty) {
    if (ty.kind != PIR_TYPE_NAMED_AGG || !mod || mod->agg_types.len == 0u) {
        return pir_type_layout(mod, target, ty).align;
    }
    uint32_t stack_cache[64];
    uint32_t *cache = (mod->agg_types.len <= 64u)
                      ? stack_cache
                      : (uint32_t *)calloc(mod->agg_types.len, sizeof(uint32_t));
    if (!cache) abort();
    if (cache == stack_cache) memset(stack_cache, 0, mod->agg_types.len * sizeof(uint32_t));
    uint32_t r = pir_abi_natural_align_rec(mod, target, ty, true, cache);
    if (cache != stack_cache) free(cache);
    return r;
}

void pir_abi_vec_param_regs(const PirTargetSpec *target, PirVecType vt,
                            uint32_t *out_gp, uint32_t *out_fp) {
    *out_gp = 0u;
    *out_fp = 0u;
    uint16_t bits = pir_vec_bits(vt);
    if (bits == 0u) return;
    uint16_t max_bits = target->caps.max_simd_bits;
    if (max_bits == 0u) {
        uint32_t lanes = pir_vec_lanes(vt);
        PirScalarType elem = pir_vec_elem_scalar(vt);
        if (pir_scalar_is_float(elem)) {
            *out_fp = lanes;
        } else {
            *out_gp = lanes;
        }
    } else if (bits <= max_bits) {
        *out_fp = 1u;
    } else {
        *out_fp = (uint32_t)((bits + max_bits - 1u) / max_bits);
    }
}

bool pir_abi_classify_param(const PirModule *mod, const PirTargetSpec *target,
                            const PirParamDecl *pd, PirAbiRegState *st,
                            PirAbiArgInfo *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (target->arch == PIR_ARCH_X86_64 && target->os == PIR_OS_WINDOWS) {
        return pir_abi_win64_param(mod, target, pd, st, out);
    }
    if (target->arch == PIR_ARCH_X86_64) {
        return pir_abi_sysv_x64_param(mod, target, pd, st, out);
    }
    if (target->arch == PIR_ARCH_ARM64) {
        return pir_abi_aapcs64_param(mod, target, pd, st, out);
    }
    return false;
}

bool pir_abi_classify_retbuf(const PirModule *mod, const PirTargetSpec *target,
                             PirType agg_ty, PirAbiArgInfo *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (target->arch == PIR_ARCH_X86_64 && target->os == PIR_OS_WINDOWS) {
        return pir_abi_win64_retbuf(mod, target, agg_ty, out);
    }
    if (target->arch == PIR_ARCH_X86_64) {
        return pir_abi_sysv_x64_retbuf(mod, target, agg_ty, out);
    }
    if (target->arch == PIR_ARCH_ARM64) {
        return pir_abi_aapcs64_retbuf(mod, target, agg_ty, out);
    }
    return false;
}

static uint64_t abi_align_up_u64(uint64_t v, uint64_t al) {
    return (v + al - 1u) & ~(al - 1u);
}

void pir_abi_plan_va_arg(const PirModule *mod, const PirTargetSpec *target,
                         PirType arg_ty, bool is_buf, PirVaArgPlan *out) {
    memset(out, 0, sizeof(*out));
    out->arg_ty      = arg_ty;
    out->is_buf      = is_buf;
    out->promote_f32 = (!is_buf && arg_ty.kind == PIR_TYPE_SCALAR &&
                        arg_ty.u.scalar == PIR_SCALAR_F32);
    out->load_ty     = out->promote_f32 ? pir_type_scalar(PIR_SCALAR_F64) : arg_ty;
    PirTypeLayout ly = pir_type_layout(mod, target, out->load_ty);
    out->size        = ly.size;
    out->align       = ly.align;
    out->nat_align   = pir_abi_natural_align(mod, target, out->load_ty);

    PirParamDecl pd;
    memset(&pd, 0, sizeof(pd));
    if (is_buf) {
        pd.abi_kind = PIR_PARAM_BYVAL;
        pd.abi_type = arg_ty;
        pd.type     = pir_type_scalar(PIR_SCALAR_PTR);
    } else {
        pd.abi_kind = PIR_PARAM_NORMAL;
        pd.type     = out->load_ty;
    }
    PirAbiRegState st0 = { 0u, 0u };
    out->abi_valid = pir_abi_classify_param(mod, target, &pd, &st0, &out->abi);

    out->is_empty = is_buf &&
                    (out->size == 0u ||
                     (out->abi_valid && out->abi.kind == PIR_ABI_DIRECT && out->abi.reg_count == 0u));

    out->is_indirect = out->abi_valid &&
                       out->abi.kind == PIR_ABI_INDIRECT &&
                       out->abi.stack_byval_size == 0u;

    bool is_sysv_x64    = (target->arch == PIR_ARCH_X86_64 && target->os != PIR_OS_WINDOWS);
    bool is_aapcs64_lin = (target->arch == PIR_ARCH_ARM64  && target->os != PIR_OS_MACOS);
    bool is_darwin_a64  = (target->arch == PIR_ARCH_ARM64  && target->os == PIR_OS_MACOS);

    if (is_sysv_x64) {
        if (is_buf) {
            out->force_mem = !(out->abi_valid && out->abi.kind == PIR_ABI_DIRECT &&
                               out->abi.reg_count > 0u &&
                               (out->abi.gp_used > 0u || out->abi.fp_used > 0u));
        } else {
            /* SysV AMD64 §3.5.7: 256-bit vectors always come from overflow_arg_area. */
            out->force_mem = (out->load_ty.kind == PIR_TYPE_VECTOR && out->size > 16u);
        }
    }

    out->is_hfa = (is_aapcs64_lin && is_buf && !out->is_indirect &&
                   out->abi_valid && out->abi.kind == PIR_ABI_DIRECT && out->abi.fp_used > 0u);
    out->is_fp  = out->is_hfa ||
                  (!is_buf && !out->is_indirect &&
                   (pir_type_is_float(out->load_ty) || out->load_ty.kind == PIR_TYPE_VECTOR));

    out->split_vec = (!is_buf && out->load_ty.kind == PIR_TYPE_VECTOR &&
                      pir_vec_bits(out->load_ty.u.vec) > target->caps.max_simd_bits &&
                      !out->is_indirect && !out->force_mem);

    uint64_t slot_sz = (target->ptr_bytes == 4u) ? 4u : 8u;
    out->mem_step = out->is_indirect
                    ? slot_sz
                    : abi_align_up_u64(out->size > 0u ? out->size : slot_sz, slot_sz);

    out->mem_align = slot_sz;
    if (is_sysv_x64) {
        if (out->align > 8u) {
            out->mem_align = (is_buf && out->align < 16u) ? 16u : out->align;
        }
    } else if (is_aapcs64_lin && !out->is_indirect) {
        uint32_t stk_al = out->is_hfa ? out->align : out->nat_align;
        if (stk_al > 8u) out->mem_align = (stk_al > 16u) ? 16u : stk_al;
    } else if (is_darwin_a64 && !out->is_indirect) {
        if (out->align > 8u) out->mem_align = (out->align > 16u) ? 16u : out->align;
    }

    out->align_gp16 = (is_aapcs64_lin && !out->is_fp && !out->is_indirect && out->nat_align >= 16u);
}
