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
    out->caps.max_int_bits  = 64u;
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
    if (strcmp(triple, "riscv32-unknown-elf") == 0 ||
        strcmp(triple, "riscv32-unknown-linux-gnu") == 0 ||
        strcmp(triple, "riscv32-linux-gnu") == 0) {
        out->arch               = PIR_ARCH_RV32;
        out->os                 = strstr(triple, "linux") ? PIR_OS_LINUX : PIR_OS_BAREMETAL;
        out->ptr_bytes          = 4u;
        out->is_big_endian      = false;
        out->va_list_size       = 4u;
        out->va_list_align      = 4u;
        out->caps.max_simd_bits = 0u;
        out->caps.max_int_bits  = 32u;
        out->caps.has_mul_wide  = true;  /* RV32M mul + mulh/mulhu (32x32->64) */
        out->caps.has_add_carry = false; /* Flagless ISA */
        out->caps.has_ovf_ops   = false; /* Flagless ISA */
        out->caps.has_bitops    = false; /* Base RV32IMAFD (no Zbb) */
        out->caps.has_switch    = true;
        return true;
    }
    if (strcmp(triple, "powerpc-unknown-elf") == 0 ||
        strcmp(triple, "powerpc-unknown-linux-gnu") == 0 ||
        strcmp(triple, "powerpc-linux-gnu") == 0 ||
        strcmp(triple, "ppc32-unknown-elf") == 0) {
        out->arch               = PIR_ARCH_PPC32;
        out->os                 = strstr(triple, "linux") ? PIR_OS_LINUX : PIR_OS_BAREMETAL;
        out->ptr_bytes          = 4u;
        out->is_big_endian      = true;
        out->va_list_size       = 12u;
        out->va_list_align      = 4u;
        out->caps.max_simd_bits = 0u;
        out->caps.max_int_bits  = 32u;
        out->caps.has_mul_wide  = true;  /* PPC32 mullw + mulhw/mulhwu (32x32->64) */
        out->caps.has_add_carry = false;
        out->caps.has_ovf_ops   = false;
        out->caps.has_bitops    = false;
        out->caps.has_switch    = true;
        return true;
    }
    if (strcmp(triple, "wasm32-unknown-unknown") == 0 ||
        strcmp(triple, "i386-unknown-linux-gnu") == 0 ||
        strcmp(triple, "i686-unknown-linux-gnu") == 0) {
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
        if (spec->caps.max_simd_bits > 0u) {
            spec->caps.max_simd_bits = 256u;
        }
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

const char *pir_target_elf_builtin_section_dir(const char *name) {
    if (!name) return NULL;
    static const struct { const char *name; const char *dir; } BUILTIN_SECS[] = {
        { ".text",           "\t.text\n" },
        { ".data",           "\t.data\n" },
        { ".rodata",         "\t.section .rodata,\"a\",@progbits\n" },
        { ".data.rel.ro",    "\t.section .data.rel.ro,\"aw\",@progbits\n" },
        { ".tdata",          "\t.section .tdata,\"awT\",@progbits\n" },
        { ".bss",            "\t.bss\n" },
        { ".tbss",           "\t.section .tbss,\"awT\",@nobits\n" },
        { ".eh_frame",       "\t.section .eh_frame,\"a\",@progbits\n" },
        { ".note.GNU-stack", "\t.section .note.GNU-stack,\"\",@progbits\n" },
    };
    for (size_t i = 0u; i < sizeof(BUILTIN_SECS) / sizeof(BUILTIN_SECS[0]); ++i) {
        if (strcmp(name, BUILTIN_SECS[i].name) == 0) {
            return BUILTIN_SECS[i].dir;
        }
    }
    return NULL;
}

static bool target_is_elf_predefined_sec(const char *name) {
    static const char *const PREDEF[] = {
        ".rodata.cst4", ".rodata.cst8", ".rodata.cst16", ".rodata.cst32",
        ".debug_line", ".debug_info", ".debug_abbrev", ".debug_str",
        ".debug_ranges", ".debug_aranges", ".debug_frame", ".debug_loc",
        ".debug_macinfo", ".debug_pubnames", ".debug_pubtypes",
        ".symtab", ".strtab", ".shstrtab", ".riscv.attributes"
    };
    for (size_t i = 0u; i < sizeof(PREDEF) / sizeof(PREDEF[0]); ++i) {
        if (strcmp(name, PREDEF[i]) == 0) return true;
    }
    return false;
}

bool pir_target_is_elf_reserved_sym(const PirTargetSpec *target, const char *name) {
    if (!name) return false;
    if (pir_target_elf_builtin_section_dir(name) != NULL) return true;
    if (target_is_elf_predefined_sec(name)) return true;
    if (strncmp(name, ".rela.", 6u) == 0 || strncmp(name, ".rel.", 5u) == 0) return true;
    (void)target;
    return false;
}

static bool target_is_macho_ident_start(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool target_is_macho_ident_char(unsigned char c, bool allow_plus) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' ||
           (allow_plus && c == '+');
}

static bool target_is_macho_section_type(const char *s, size_t len) {
    static const char *const TYPES[] = {
        "regular", "zerofill", "cstring_literals", "4byte_literals",
        "8byte_literals", "16byte_literals", "literal_pointers",
        "non_lazy_symbol_pointers", "lazy_symbol_pointers",
        "mod_init_funcs", "mod_term_funcs", "coalesced",
        "interposing",
        "thread_local_regular", "thread_local_zerofill", "thread_local_variables",
        "thread_local_variable_pointers", "thread_local_init_function_pointers"
    };
    for (size_t i = 0u; i < sizeof(TYPES) / sizeof(TYPES[0]); ++i) {
        if (strlen(TYPES[i]) == len && memcmp(s, TYPES[i], len) == 0) return true;
    }
    return false;
}

static bool target_is_macho_section_attr(const char *s, size_t len) {
    static const char *const ATTRS[] = {
        "pure_instructions", "no_toc", "strip_static_syms", "no_dead_strip",
        "live_support", "self_modifying_code", "debug", "some_instructions",
        "ext_reloc", "loc_reloc"
    };
    for (size_t i = 0u; i < sizeof(ATTRS) / sizeof(ATTRS[0]); ++i) {
        if (strlen(ATTRS[i]) == len && memcmp(s, ATTRS[i], len) == 0) return true;
    }
    return false;
}

bool pir_target_macho_seg_sec_key(const char *sec, char *buf, size_t cap) {
    if (!sec || !buf || cap == 0u) return false;
    const char *c1 = strchr(sec, ',');
    if (!c1) return false;
    const char *c2 = strchr(c1 + 1, ',');
    size_t len = c2 ? (size_t)(c2 - sec) : strlen(sec);
    if (len == 0u || len + 1u > cap) return false;
    memcpy(buf, sec, len);
    buf[len] = '\0';
    return true;
}

bool pir_target_section_is_nobits(const PirTargetSpec *target, const char *sec) {
    if (!target || !sec) return false;
    if (target->os == PIR_OS_MACOS) {
        const char *c1 = strchr(sec, ',');
        if (!c1) return false;
        size_t l0 = (size_t)(c1 - sec);
        const char *p1 = c1 + 1;
        const char *c2 = strchr(p1, ',');
        size_t l1 = c2 ? (size_t)(c2 - p1) : strlen(p1);
        if (l0 == 6u && memcmp(sec, "__DATA", 6u) == 0 &&
            ((l1 == 5u && memcmp(p1, "__bss", 5u) == 0) ||
             (l1 == 8u && memcmp(p1, "__common", 8u) == 0) ||
             (l1 == 12u && memcmp(p1, "__thread_bss", 12u) == 0))) {
            return true;
        }
        if (c2) {
            const char *p2 = c2 + 1;
            const char *c3 = strchr(p2, ',');
            size_t l2 = c3 ? (size_t)(c3 - p2) : strlen(p2);
            if ((l2 == 8u && memcmp(p2, "zerofill", 8u) == 0) ||
                (l2 == 21u && memcmp(p2, "thread_local_zerofill", 21u) == 0)) {
                return true;
            }
        }
        return false;
    }
    if (target->os == PIR_OS_WINDOWS) return false;
    return strcmp(sec, ".bss") == 0 || strcmp(sec, ".tbss") == 0;
}

const char *pir_target_validate_section(const PirTargetSpec *target, const char *sec) {
    if (!target || !sec) return NULL;
    if (target->os == PIR_OS_MACOS) {
        if (strchr(sec, ';') != NULL) return "invalid Mach-O section name";
        if (strchr(sec, ',') == NULL) return NULL;
        const char *f[4] = { NULL, NULL, NULL, NULL };
        size_t      fl[4] = { 0u, 0u, 0u, 0u };
        uint32_t    nf = 0u;
        const char *p = sec;
        for (;;) {
            if (nf >= 4u) return "Mach-O section has too many comma-separated fields";
            const char *c = strchr(p, ',');
            size_t len = c ? (size_t)(c - p) : strlen(p);
            if (len == 0u) return "Mach-O section has empty field";
            if (nf == 0u && !target_is_macho_ident_start((unsigned char)p[0])) {
                return "Mach-O segment name must start with an identifier character";
            }
            for (size_t k = 0u; k < len; ++k) {
                if (!target_is_macho_ident_char((unsigned char)p[k], nf == 3u)) {
                    return "Mach-O section contains invalid character";
                }
            }
            f[nf]  = p;
            fl[nf] = len;
            nf++;
            if (!c) break;
            p = c + 1;
        }
        if (fl[0] > 16u || fl[1] > 16u) return "Mach-O segment/section name exceeds 16 characters";
        if (nf >= 3u && !target_is_macho_section_type(f[2], fl[2])) return "unknown Mach-O section type";
        if (nf >= 4u) {
            const char *ap = f[3];
            const char *ae = f[3] + fl[3];
            while (ap < ae) {
                const char *plus = (const char *)memchr(ap, '+', (size_t)(ae - ap));
                size_t alen = plus ? (size_t)(plus - ap) : (size_t)(ae - ap);
                if (alen == 0u || !target_is_macho_section_attr(ap, alen)) {
                    return "unknown Mach-O section attribute";
                }
                if (!plus) break;
                ap = plus + 1;
                if (ap == ae) return "trailing '+' in Mach-O section attributes";
            }
        }
        return NULL;
    }
    if (target->os == PIR_OS_WINDOWS) return NULL;
    if (target_is_elf_predefined_sec(sec)) {
        return "ELF predefined/reserved section name cannot be used as custom section";
    }
    for (const char *p = sec; *p; ++p) {
        if (*p == '\\' || *p == '"') return "ELF section name cannot contain backslash or quote";
    }
    return NULL;
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
    if (target->arch == PIR_ARCH_RV32) {
        return pir_abi_rv32_param(mod, target, pd, st, out);
    }
    if (target->arch == PIR_ARCH_PPC32) {
        return pir_abi_ppc32_param(mod, target, pd, st, out);
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
    if (target->arch == PIR_ARCH_RV32) {
        return pir_abi_rv32_retbuf(mod, target, agg_ty, out);
    }
    if (target->arch == PIR_ARCH_PPC32) {
        return pir_abi_ppc32_retbuf(mod, target, agg_ty, out);
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
    if (target->arch == PIR_ARCH_RV32 && is_buf && !out->is_empty && out->size > 8u) {
        out->is_indirect = true;
    }

    bool is_sysv_x64    = (target->arch == PIR_ARCH_X86_64 && target->os != PIR_OS_WINDOWS);
    bool is_aapcs64_lin = (target->arch == PIR_ARCH_ARM64  && target->os != PIR_OS_MACOS);
    bool is_darwin_a64  = (target->arch == PIR_ARCH_ARM64  && target->os == PIR_OS_MACOS);

    if (is_sysv_x64) {
        if (is_buf) {
            out->force_mem = !(out->abi_valid && out->abi.kind == PIR_ABI_DIRECT &&
                               out->abi.reg_count > 0u && out->size <= 16u &&
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
        if (is_buf && out->abi_valid && out->abi.kind == PIR_ABI_INDIRECT &&
            out->abi.stack_byval_align > 0u) {
            out->mem_align = (out->abi.stack_byval_align > 8u) ? out->abi.stack_byval_align : 8u;
        } else if (out->align > 8u) {
            uint64_t mal = (is_buf && out->align < 16u) ? 16u : out->align;
            if (target->os == PIR_OS_MACOS && target->caps.max_simd_bits < 256u &&
                !is_buf && out->load_ty.kind == PIR_TYPE_VECTOR && out->size > 16u && mal > 16u) {
                mal = 16u;
            }
            out->mem_align = mal;
        }
    } else if (is_aapcs64_lin && !out->is_indirect) {
        uint32_t stk_al = out->nat_align;
        if (stk_al > 8u) out->mem_align = (stk_al > 16u) ? 16u : stk_al;
    } else if (is_darwin_a64 && !out->is_indirect) {
        if (out->align > 8u) out->mem_align = (out->align > 16u) ? 16u : out->align;
    } else if ((target->arch == PIR_ARCH_RV32 || target->arch == PIR_ARCH_PPC32) && !out->is_indirect) {
        /* 32-bit RISC-V ILP32D & SysV PPC32: 8B aligned variadic args align cursor to 8 bytes. */
        if (out->align >= 8u) out->mem_align = 8u;
    }

    out->align_gp16 = (is_aapcs64_lin && !out->is_fp && !out->is_indirect && out->nat_align >= 16u);
}
