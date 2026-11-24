/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "target/target.h"
#include "target/abi.h"

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

bool pir_abi_classify_param(const PirModule *mod, const PirTargetSpec *target,
                            const PirParamDecl *pd, PirAbiRegState *st,
                            PirAbiArgInfo *out) {
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
