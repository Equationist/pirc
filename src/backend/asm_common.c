/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/asm_common.h"

#include <inttypes.h>
#include <string.h>

#include "lower/lower.h"

bool pir_asm_is_macho(const PirTargetSpec *target) {
    return target->os == PIR_OS_MACOS;
}

const char *pir_asm_sym_pfx(const PirTargetSpec *target) {
    return pir_asm_is_macho(target) ? "_" : "";
}

const char *pir_asm_lbl_pfx(const PirTargetSpec *target) {
    return pir_asm_is_macho(target) ? "L" : ".L";
}

uint32_t pir_asm_log2_align(uint32_t align) {
    uint32_t p = 0u;
    while (align > 1u) {
        align >>= 1u;
        p++;
    }
    return p;
}

MCond pir_asm_invert_cond(MCond c) {
    switch (c) {
        case MCOND_EQ: return MCOND_NE;
        case MCOND_NE: return MCOND_EQ;
        case MCOND_CS: return MCOND_CC;
        case MCOND_CC: return MCOND_CS;
        case MCOND_MI: return MCOND_PL;
        case MCOND_PL: return MCOND_MI;
        case MCOND_VS: return MCOND_VC;
        case MCOND_VC: return MCOND_VS;
        case MCOND_HI: return MCOND_LS;
        case MCOND_LS: return MCOND_HI;
        case MCOND_GE: return MCOND_LT;
        case MCOND_LT: return MCOND_GE;
        case MCOND_GT: return MCOND_LE;
        case MCOND_LE: return MCOND_GT;
        case MCOND_P:  return MCOND_NP;
        case MCOND_NP: return MCOND_P;
    }
    return MCOND_NE;
}

void pir_asm_emit_escaped_cstr(FILE *out, const char *s) {
    fputc('"', out);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
            unsigned char c = *p;
            if (c == '"' || c == '\\') {
                fputc('\\', out);
                fputc((int)c, out);
            } else if (c == '\n') {
                fputs("\\n", out);
            } else if (c == '\r') {
                fputs("\\r", out);
            } else if (c == '\t') {
                fputs("\\t", out);
            } else if (c < 0x20u || c >= 0x7fu) {
                fprintf(out, "\\%03o", (unsigned)c);
            } else {
                fputc((int)c, out);
            }
        }
    }
    fputc('"', out);
}

static bool is_bare_asm_sym_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.';
}

void pir_asm_emit_sym(FILE *out, const PirTargetSpec *target,
                      const char *name, const char *suffix,
                      PirAsmNeedsQuoteFn arch_needs_quote) {
    const char *sp = pir_asm_sym_pfx(target);
    if (!name) name = "";
    bool has_suffix = (suffix && suffix[0] != '\0');
    bool bare = (name[0] != '\0');
    for (const char *p = name; *p && bare; ++p) {
        if (!is_bare_asm_sym_char((unsigned char)*p)) bare = false;
    }
    if (bare && sp[0] == '\0') {
        /* Without a leading '_', identifiers starting with a digit or '$', or equal to ".",
         * or colliding with target register/keyword names must be quoted for gas/LLVM-as. */
        unsigned char c0 = (unsigned char)name[0];
        if ((c0 >= '0' && c0 <= '9') || c0 == '$' ||
            (!has_suffix && strcmp(name, ".") == 0) ||
            (!has_suffix && arch_needs_quote && arch_needs_quote(name))) {
            bare = false;
        }
    }
    if (bare) {
        fputs(sp, out);
        fputs(name, out);
        if (has_suffix) fputs(suffix, out);
        return;
    }
    fputc('"', out);
    fputs(sp, out);
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            fputc('\\', out);
            fputc((int)c, out);
        } else if (c < 0x20u || c >= 0x7fu) {
            fprintf(out, "\\%03o", (unsigned)c);
        } else {
            fputc((int)c, out);
        }
    }
    if (has_suffix) fputs(suffix, out);
    fputc('"', out);
}

static bool elf_sec_needs_quote(const char *s) {
    if (!s || !s[0]) return true;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$' || c == '-';
        if (!ok) return true;
    }
    return false;
}

static void emit_elf_section_name(FILE *out, const char *sec) {
    if (elf_sec_needs_quote(sec)) {
        pir_asm_emit_escaped_cstr(out, sec);
    } else {
        fputs(sec, out);
    }
}

void pir_asm_emit_section_directive(FILE *out, const PirTargetSpec *target,
                                    const char *custom_sec, PirGlobalSeg seg,
                                    bool has_reloc, bool is_zero, bool is_func) {
    if (custom_sec && custom_sec[0] != '\0') {
        if (!pir_asm_is_macho(target)) {
            if (strchr(custom_sec, ',') == NULL) {
                bool force_nobits = (strcmp(custom_sec, ".bss") == 0 ||
                                     strcmp(custom_sec, ".tbss") == 0);
                fputs("\t.section ", out);
                emit_elf_section_name(out, custom_sec);
                if (is_func) {
                    fputs(",\"ax\",@progbits\n", out);
                } else if (seg == PIR_GSEG_TLS) {
                    fprintf(out, ",\"awT\",%s\n", (is_zero || force_nobits) ? "@nobits" : "@progbits");
                } else if (seg == PIR_GSEG_RODATA && !has_reloc && !force_nobits) {
                    fputs(",\"a\",@progbits\n", out);
                } else {
                    fprintf(out, ",\"aw\",%s\n", (is_zero || force_nobits) ? "@nobits" : "@progbits");
                }
            } else {
                fprintf(out, "\t.section %s\n", custom_sec);
            }
            return;
        }
        if (strchr(custom_sec, ',') != NULL) {
            fprintf(out, "\t.section %s\n", custom_sec);
            return;
        }
        if (is_func) {
            fprintf(out, "\t.section __TEXT,__text,regular,pure_instructions\n");
            return;
        }
        /* Map ELF-style custom section names to valid Mach-O SEGMENT,section syntax. */
        if (strncmp(custom_sec, ".rodata", 7u) == 0) {
            fprintf(out, "\t.section %s\n", has_reloc ? "__DATA_CONST,__const" : "__TEXT,__const");
        } else if (strncmp(custom_sec, ".text", 5u) == 0) {
            fprintf(out, "\t.section __TEXT,__text,regular,pure_instructions\n");
        } else if (strncmp(custom_sec, ".bss", 4u) == 0 && is_zero) {
            fprintf(out, "\t.section __DATA,__bss\n");
        } else {
            fprintf(out, "\t.section __DATA,__data\n");
        }
        return;
    }
    if (pir_asm_is_macho(target)) {
        switch (seg) {
            case PIR_GSEG_RODATA:
                fprintf(out, "\t.section %s\n", has_reloc ? "__DATA_CONST,__const" : "__TEXT,__const");
                break;
            case PIR_GSEG_DATA:
                fprintf(out, "\t.section __DATA,__data\n");
                break;
            case PIR_GSEG_TLS:
                fprintf(out, "\t.section __DATA,__thread_data,thread_local_regular\n");
                break;
        }
    } else {
        switch (seg) {
            case PIR_GSEG_RODATA:
                fprintf(out, has_reloc ? "\t.section .data.rel.ro,\"aw\",@progbits\n"
                                       : "\t.section .rodata\n");
                break;
            case PIR_GSEG_DATA:
                fprintf(out, is_zero ? "\t.bss\n" : "\t.data\n");
                break;
            case PIR_GSEG_TLS:
                fprintf(out, is_zero ? "\t.section .tbss,\"awT\",@nobits\n"
                                     : "\t.section .tdata,\"awT\",@progbits\n");
                break;
        }
    }
}

void pir_asm_emit_dwarf_files(FILE *out, const PirModule *mod) {
    for (uint32_t i = 0u; i < mod->files.len; ++i) {
        fprintf(out, "\t.file %u ", i + 1u);
        pir_asm_emit_escaped_cstr(out, mod->files.data[i].path);
        fputc('\n', out);
    }
}

uint32_t pir_asm_dwarf_file_num(const PirModule *mod, PirFileId fid) {
    for (uint32_t i = 0u; i < mod->files.len; ++i) {
        if (mod->files.data[i].id == fid) {
            return i + 1u;
        }
    }
    return 1u;
}

static bool global_has_reloc(const PirGlobal *g) {
    for (uint32_t k = 0u; k < g->item_count; ++k) {
        if (g->items[k].kind == PIR_GITEM_RELOC) return true;
    }
    return false;
}

static uint64_t compute_global_bytes(const PirModule *mod, const PirTargetSpec *target,
                                     const PirGlobal *g) {
    if (g->has_type && (g->type.kind == PIR_TYPE_SCALAR || g->type.kind == PIR_TYPE_VECTOR)) {
        PirTypeLayout ly = pir_type_layout(mod, target, g->type);
        return ly.size > 0u ? ly.size : 1u;
    }
    uint64_t tot = 0u;
    for (uint32_t k = 0u; k < g->item_count; ++k) {
        const PirGlobalItem *gi = &g->items[k];
        switch (gi->kind) {
            case PIR_GITEM_BYTES: tot += gi->len; break;
            case PIR_GITEM_ZERO:  tot += gi->len > 0u ? gi->len : 1u; break;
            case PIR_GITEM_INT:   tot += 1u; break;
            case PIR_GITEM_FLOAT: tot += 8u; break;
            case PIR_GITEM_RELOC: tot += target->ptr_bytes; break;
        }
    }
    return tot > 0u ? tot : 1u;
}

static bool global_is_all_zero(const PirGlobal *g) {
    if (g->is_zeroinit) return true;
    if (g->item_count == 0u) return true;
    if (g->has_type && g->type.kind == PIR_TYPE_VECTOR) {
        bool is_fp = pir_scalar_is_float(pir_vec_elem_scalar(g->type.u.vec));
        for (uint32_t k = 0u; k < g->item_count; ++k) {
            const PirGlobalItem *gi = &g->items[k];
            if (gi->kind == PIR_GITEM_INT && gi->int_lo == 0u && gi->int_hi == 0u &&
                !(is_fp && gi->int_neg)) continue;
            if (gi->kind == PIR_GITEM_FLOAT && gi->float_bits == 0u) continue;
            if (gi->kind == PIR_GITEM_ZERO) continue;
            return false;
        }
        return true;
    }
    if (g->has_type && g->type.kind == PIR_TYPE_SCALAR && g->item_count == 1u) {
        const PirGlobalItem *gi = &g->items[0];
        bool is_fp = pir_scalar_is_float(g->type.u.scalar);
        if (gi->kind == PIR_GITEM_INT && gi->int_lo == 0u && gi->int_hi == 0u &&
            !(is_fp && gi->int_neg)) return true;
        if (gi->kind == PIR_GITEM_FLOAT && gi->float_bits == 0u) return true;
        if (gi->kind == PIR_GITEM_ZERO) return true;
        return false;
    }
    for (uint32_t k = 0u; k < g->item_count; ++k) {
        if (g->items[k].kind != PIR_GITEM_ZERO) return false;
    }
    return true;
}

static void emit_reloc_quad(FILE *out, const PirTargetSpec *target,
                            const PirGlobalItem *gi, PirAsmNeedsQuoteFn arch_needs_quote) {
    fputs("\t.quad ", out);
    pir_asm_emit_sym(out, target, gi->sym_name, NULL, arch_needs_quote);
    if (gi->sym_offset != 0) {
        fprintf(out, " + %" PRId64 "\n", gi->sym_offset);
    } else {
        fputc('\n', out);
    }
}

static void emit_scalar_gitem(FILE *out, const PirTargetSpec *target, PirScalarType sc,
                              uint64_t sz, const PirGlobalItem *gi,
                              PirAsmNeedsQuoteFn arch_needs_quote) {
    if (gi->kind == PIR_GITEM_INT) {
        if (sc == PIR_SCALAR_F32) {
            float fv = gi->int_neg ? -(float)gi->int_lo : (float)gi->int_lo;
            uint32_t fb = 0u;
            memcpy(&fb, &fv, 4u);
            fprintf(out, "\t.long %" PRIu32 "\n", fb);
        } else if (sc == PIR_SCALAR_F64) {
            double dv = gi->int_neg ? -(double)gi->int_lo : (double)gi->int_lo;
            uint64_t db = 0u;
            memcpy(&db, &dv, 8u);
            fprintf(out, "\t.quad %" PRIu64 "\n", db);
        } else {
            uint64_t x = gi->int_neg ? (UINT64_C(0) - gi->int_lo) : gi->int_lo;
            if (sz == 1u)      fprintf(out, "\t.byte %" PRIu64 "\n", x & 0xFFu);
            else if (sz == 2u) fprintf(out, "\t.short %" PRIu64 "\n", x & 0xFFFFu);
            else if (sz == 4u) fprintf(out, "\t.long %" PRIu64 "\n", x & 0xFFFFFFFFu);
            else               fprintf(out, "\t.quad %" PRIu64 "\n", x);
        }
    } else if (gi->kind == PIR_GITEM_FLOAT) {
        if (sz == 4u) fprintf(out, "\t.long %" PRIu64 "\n", gi->float_bits & 0xFFFFFFFFu);
        else          fprintf(out, "\t.quad %" PRIu64 "\n", gi->float_bits);
    } else if (gi->kind == PIR_GITEM_RELOC) {
        emit_reloc_quad(out, target, gi, arch_needs_quote);
    } else if (gi->kind == PIR_GITEM_ZERO) {
        fprintf(out, "\t.zero %" PRIu64 "\n", gi->len > 0u ? gi->len : sz);
    }
}

static void emit_global_data_items(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                                   const PirGlobal *g, PirAsmNeedsQuoteFn arch_needs_quote) {
    if (g->is_zeroinit) {
        uint64_t sz = compute_global_bytes(mod, target, g);
        fprintf(out, "\t.zero %" PRIu64 "\n", sz);
        return;
    }
    if (g->has_type && g->type.kind == PIR_TYPE_VECTOR) {
        if (g->item_count == 1u && g->items[0].kind == PIR_GITEM_ZERO) {
            uint64_t sz = compute_global_bytes(mod, target, g);
            fprintf(out, "\t.zero %" PRIu64 "\n", sz);
            return;
        }
        PirScalarType elem = pir_vec_elem_scalar(g->type.u.vec);
        uint32_t lanes = pir_vec_lanes(g->type.u.vec);
        uint64_t esz = pir_scalar_bits(elem, target->ptr_bytes) / 8u;
        for (uint32_t l = 0u; l < lanes; ++l) {
            if (l < g->item_count) {
                emit_scalar_gitem(out, target, elem, esz, &g->items[l], arch_needs_quote);
            } else {
                fprintf(out, "\t.zero %" PRIu64 "\n", esz);
            }
        }
        return;
    }
    if (g->has_type && g->type.kind == PIR_TYPE_SCALAR && g->item_count == 1u) {
        PirTypeLayout ly = pir_type_layout(mod, target, g->type);
        emit_scalar_gitem(out, target, g->type.u.scalar, ly.size, &g->items[0], arch_needs_quote);
        return;
    }

    for (uint32_t k = 0u; k < g->item_count; ++k) {
        const PirGlobalItem *gi = &g->items[k];
        switch (gi->kind) {
            case PIR_GITEM_BYTES:
                for (uint64_t bi = 0u; bi < gi->len; ++bi) {
                    fprintf(out, "\t.byte %u\n", (unsigned)gi->bytes[bi]);
                }
                break;
            case PIR_GITEM_ZERO:
                fprintf(out, "\t.zero %" PRIu64 "\n", gi->len > 0u ? gi->len : 1u);
                break;
            case PIR_GITEM_INT: {
                uint8_t b = (uint8_t)(gi->int_neg ? (UINT64_C(0) - gi->int_lo) : gi->int_lo);
                fprintf(out, "\t.byte %u\n", (unsigned)b);
                break;
            }
            case PIR_GITEM_FLOAT:
                fprintf(out, "\t.quad %" PRIu64 "\n", gi->float_bits);
                break;
            case PIR_GITEM_RELOC:
                emit_reloc_quad(out, target, gi, arch_needs_quote);
                break;
        }
    }
}

static void emit_weak_extern_sym(FILE *out, const PirTargetSpec *target,
                                 const char *name, PirAsmNeedsQuoteFn arch_needs_quote) {
    if (pir_asm_is_macho(target)) {
        fputs("\t.weak_reference ", out);
    } else {
        fputs("\t.weak ", out);
    }
    pir_asm_emit_sym(out, target, name, NULL, arch_needs_quote);
    fputc('\n', out);
}

bool pir_asm_emit_globals(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                          PirAsmNeedsQuoteFn arch_needs_quote) {
    /* Emit weak extern directives for undefined weak functions and globals. */
    bool emitted_weak_ref = false;
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        const PirFunction *fn = &mod->funcs.data[i];
        if (fn->linkage == PIR_LINK_EXTERN && fn->is_weak_extern) {
            emit_weak_extern_sym(out, target, fn->name, arch_needs_quote);
            emitted_weak_ref = true;
        }
    }
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        if (g->linkage == PIR_LINK_EXTERN && g->is_weak_extern) {
            emit_weak_extern_sym(out, target, g->name, arch_needs_quote);
            emitted_weak_ref = true;
        }
    }
    if (emitted_weak_ref) fputc('\n', out);

    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        if (g->linkage == PIR_LINK_EXTERN) continue;
        uint32_t al = g->align;
        if (al == 0u) {
            if (g->has_type) {
                PirTypeLayout ly = pir_type_layout(mod, target, g->type);
                al = ly.align > 0u ? ly.align : 1u;
            } else {
                al = 1u;
            }
        }
        uint32_t p2 = pir_asm_log2_align(al);
        bool all_zero = global_is_all_zero(g);
        uint64_t sz = compute_global_bytes(mod, target, g);

        /* Darwin Mach-O TLS: __DATA,__thread_vars descriptor + __thread_data / .tbss backing */
        if (pir_asm_is_macho(target) && g->seg == PIR_GSEG_TLS) {
            if (all_zero) {
                fputs("\t.tbss ", out);
                pir_asm_emit_sym(out, target, g->name, "$tlv$init", arch_needs_quote);
                fprintf(out, ", %" PRIu64 ", %u\n", sz, p2);
            } else {
                fprintf(out, "\t.section __DATA,__thread_data,thread_local_regular\n");
                if (p2 > 0u) fprintf(out, "\t.p2align %u\n", p2);
                pir_asm_emit_sym(out, target, g->name, "$tlv$init", arch_needs_quote);
                fputs(":\n", out);
                emit_global_data_items(out, mod, target, g, arch_needs_quote);
            }
            fprintf(out, "\t.section __DATA,__thread_vars,thread_local_variables\n");
            fprintf(out, "\t.p2align 3\n");
            if (g->linkage == PIR_LINK_EXPORT) {
                fputs("\t.globl ", out);
                pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
                fputc('\n', out);
            } else if (g->linkage == PIR_LINK_WEAK) {
                fputs("\t.globl ", out);
                pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
                fputc('\n', out);
                fputs("\t.weak_definition ", out);
                pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
                fputc('\n', out);
            }
            pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
            fputs(":\n", out);
            fprintf(out, "\t.quad __tlv_bootstrap\n");
            fprintf(out, "\t.quad 0\n");
            fputs("\t.quad ", out);
            pir_asm_emit_sym(out, target, g->name, "$tlv$init", arch_needs_quote);
            fputs("\n\n", out);
            continue;
        }

        /* Mach-O internal zeroinit mutable data in default section -> .zerofill __DATA,__bss */
        if (pir_asm_is_macho(target) && g->seg == PIR_GSEG_DATA && all_zero &&
            (!g->section || g->section[0] == '\0') && g->linkage == PIR_LINK_INTERNAL) {
            fputs("\t.zerofill __DATA,__bss,", out);
            pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
            fprintf(out, ",%" PRIu64 ",%u\n\n", sz, p2);
            continue;
        }

        bool has_rel = global_has_reloc(g);
        PirGlobalSeg eff_seg = g->seg;
        bool eff_zero = all_zero;
        bool eff_rel  = has_rel;
        if (!pir_asm_is_macho(target) && g->section && g->section[0] != '\0' &&
            strchr(g->section, ',') == NULL) {
            /* Merge ELF section flags/type across all globals sharing the same custom section
             * so GAS/LLVM-MC never rejects conflicting flags or @nobits/@progbits changes. */
            for (uint32_t j = 0u; j < mod->globals.len; ++j) {
                const PirGlobal *og = &mod->globals.data[j];
                if (og->linkage == PIR_LINK_EXTERN || !og->section ||
                    strcmp(og->section, g->section) != 0) {
                    continue;
                }
                if (!global_is_all_zero(og)) eff_zero = false;
                if (global_has_reloc(og)) eff_rel = true;
                if (og->seg == PIR_GSEG_TLS) eff_seg = PIR_GSEG_TLS;
                else if (og->seg == PIR_GSEG_DATA && eff_seg == PIR_GSEG_RODATA) eff_seg = PIR_GSEG_DATA;
            }
        }
        pir_asm_emit_section_directive(out, target, g->section, eff_seg, eff_rel, eff_zero, false);

        if (g->linkage == PIR_LINK_EXPORT) {
            fputs("\t.globl ", out);
            pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
            fputc('\n', out);
        } else if (g->linkage == PIR_LINK_WEAK) {
            if (pir_asm_is_macho(target)) {
                fputs("\t.globl ", out);
                pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
                fputc('\n', out);
                fputs("\t.weak_definition ", out);
                pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
                fputc('\n', out);
            } else {
                fputs("\t.weak ", out);
                pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
                fputc('\n', out);
            }
        }
        if (p2 > 0u) fprintf(out, "\t.p2align %u\n", p2);
        pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
        fputs(":\n", out);
        emit_global_data_items(out, mod, target, g, arch_needs_quote);
        fputc('\n', out);
    }
    return true;
}

bool pir_asm_emit_module_footer(FILE *out, const PirModule *mod, const PirTargetSpec *target) {
    const char *lp = pir_asm_lbl_pfx(target);

    if (mod->files.len > 0u && pir_asm_is_macho(target)) {
        /* Minimal DWARF v4 CU + subprograms so macOS ld64 & lldb bind .loc line tables! */
        fprintf(out, "\t.section __DWARF,__debug_abbrev,regular,debug\n");
        fprintf(out, "%spir_abbrev:\n", lp);
        fprintf(out, "\t.byte 1, 17, 1\n");
        fprintf(out, "\t.byte 3, 8\n");
        fprintf(out, "\t.byte 27, 8\n");
        fprintf(out, "\t.byte 16, 23\n");
        fprintf(out, "\t.byte 0, 0\n");
        fprintf(out, "\t.byte 2, 46, 0\n");
        fprintf(out, "\t.byte 17, 1\n");
        fprintf(out, "\t.byte 18, 6\n");
        fprintf(out, "\t.byte 3, 8\n");
        fprintf(out, "\t.byte 0, 0\n");
        fprintf(out, "\t.byte 0\n");

        fprintf(out, "\t.section __DWARF,__debug_info,regular,debug\n");
        fprintf(out, "\t.long %spir_cu_end - %spir_cu_start\n", lp, lp);
        fprintf(out, "%spir_cu_start:\n", lp);
        fprintf(out, "\t.short 4\n");
        fprintf(out, "\t.long 0\n");
        fprintf(out, "\t.byte 8\n");
        fprintf(out, "\t.byte 1\n");
        fputs("\t.asciz ", out);
        pir_asm_emit_escaped_cstr(out, mod->files.data[0].path);
        fputc('\n', out);
        fprintf(out, "\t.asciz \".\"\n");
        fprintf(out, "\t.long 0\n");
        for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
            const PirFunction *f = &mod->funcs.data[fi];
            if (!f->is_defined) continue;
            fprintf(out, "\t.byte 2\n");
            fprintf(out, "\t.quad %sfunc_begin%u\n", lp, fi);
            fprintf(out, "\t.long %sfunc_end%u - %sfunc_begin%u\n", lp, fi, lp, fi);
            fputs("\t.asciz ", out);
            pir_asm_emit_escaped_cstr(out, f->name);
            fputc('\n', out);
        }
        fprintf(out, "\t.byte 0\n");
        fprintf(out, "%spir_cu_end:\n", lp);
    }

    if (pir_asm_is_macho(target)) {
        fprintf(out, ".subsections_via_symbols\n");
    } else {
        fprintf(out, "\t.section .note.GNU-stack,\"\",@progbits\n");
    }
    return true;
}
