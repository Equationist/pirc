/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "target/target.h"
#include "target/abi.h"
#include "lower/lower.h"

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
    if (ty.kind != PIR_TYPE_NAMED_AGG || ty.u.agg_id >= mod->agg_types.len) return false;
    const PirAggDef *agg = &mod->agg_types.data[ty.u.agg_id];
    if (agg->kind == PIR_AGG_PAD) return true;
    if (agg->kind == PIR_AGG_ARRAY) {
        return agg->array_len == 0u || pir_abi_is_pad_only_agg(mod, agg->elem_type);
    }
    for (uint32_t i = 0u; i < agg->field_count; ++i) {
        if (!pir_abi_is_pad_only_agg(mod, agg->fields[i])) return false;
    }
    return true;
}

static uint32_t pir_abi_natural_align_rec(const PirModule *mod, const PirTargetSpec *target,
                                          PirType ty, bool is_outer) {
    if (ty.kind != PIR_TYPE_NAMED_AGG || ty.u.agg_id >= mod->agg_types.len) {
        return pir_type_layout(mod, target, ty).align;
    }
    const PirAggDef *agg = &mod->agg_types.data[ty.u.agg_id];
    if (agg->kind == PIR_AGG_PACKED_STRUCT || agg->kind == PIR_AGG_PAD) {
        return (!is_outer && agg->min_align > 1u) ? agg->min_align : 1u;
    }
    if (agg->kind == PIR_AGG_ARRAY) {
        uint32_t ea = pir_abi_natural_align_rec(mod, target, agg->elem_type, false);
        if (!is_outer && agg->min_align > ea) ea = agg->min_align;
        return ea;
    }
    uint32_t max_al = (!is_outer && agg->min_align > 1u) ? agg->min_align : 1u;
    for (uint32_t i = 0u; i < agg->field_count; ++i) {
        uint32_t fa = pir_abi_natural_align_rec(mod, target, agg->fields[i], false);
        if (fa > max_al) max_al = fa;
    }
    return max_al;
}

uint32_t pir_abi_natural_align(const PirModule *mod, const PirTargetSpec *target, PirType ty) {
    return pir_abi_natural_align_rec(mod, target, ty, true);
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
