/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/asm_common.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "lower/lower.h"

PirObjFormat pir_asm_obj_format(const PirTargetSpec *target) {
    if (target->os == PIR_OS_MACOS)   return PIR_OBJ_MACHO;
    if (target->os == PIR_OS_WINDOWS) return PIR_OBJ_COFF;
    return PIR_OBJ_ELF;
}

bool pir_asm_is_macho(const PirTargetSpec *target) {
    return pir_asm_obj_format(target) == PIR_OBJ_MACHO;
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
        } else if (c < 0x20u || c == 0x7fu) {
            fprintf(out, "\\%03o", (unsigned)c);
        } else {
            /* Emit printable ASCII and raw UTF-8 bytes (>= 0x80) verbatim so LLVM-MC/GAS
             * preserves the exact byte sequence in Mach-O/ELF symbol tables. */
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
        /* GNU as / LLVM-MC read characters inside ELF .section "..." names verbatim
         * (no C backslash unescaping except \"), so emit bytes directly without doubling '\'. */
        fputc('"', out);
        if (sec) {
            for (const unsigned char *p = (const unsigned char *)sec; *p; ++p) {
                unsigned char c = *p;
                if (c == '"') {
                    fputs("\\\"", out);
                } else {
                    fputc((int)c, out);
                }
            }
        }
        fputc('"', out);
    } else {
        fputs(sec, out);
    }
}

static bool global_has_reloc(const PirGlobal *g);

void pir_asm_emit_linkage(FILE *out, const PirTargetSpec *target,
                          PirLinkage linkage, const char *name,
                          PirAsmNeedsQuoteFn arch_needs_quote) {
    switch (linkage) {
        case PIR_LINK_EXPORT:
            fputs("\t.globl ", out);
            pir_asm_emit_sym(out, target, name, NULL, arch_needs_quote);
            fputc('\n', out);
            break;
        case PIR_LINK_WEAK:
            if (pir_asm_obj_format(target) == PIR_OBJ_MACHO) {
                fputs("\t.globl ", out);
                pir_asm_emit_sym(out, target, name, NULL, arch_needs_quote);
                fputc('\n', out);
                fputs("\t.weak_definition ", out);
                pir_asm_emit_sym(out, target, name, NULL, arch_needs_quote);
                fputc('\n', out);
            } else {
                fputs("\t.weak ", out);
                pir_asm_emit_sym(out, target, name, NULL, arch_needs_quote);
                fputc('\n', out);
            }
            break;
        case PIR_LINK_INTERNAL:
        case PIR_LINK_EXTERN:
            break;
    }
}

static bool elf_sec_is_prefix(const char *sec, const char *pfx) {
    size_t n = strlen(pfx);
    return strncmp(sec, pfx, n) == 0 && (sec[n] == '\0' || sec[n] == '.');
}

static void emit_elf_section_directive(FILE *out, const PirModule *mod,
                                       const char *custom_sec, PirGlobalSeg seg,
                                       bool has_reloc, bool is_zero, bool is_func) {
    if (!custom_sec || custom_sec[0] == '\0') {
        if (is_func) {
            fputs("\t.text\n", out);
            return;
        }
        switch (seg) {
            case PIR_GSEG_RODATA:
                fputs(has_reloc ? "\t.section .data.rel.ro,\"aw\",@progbits\n"
                                : "\t.section .rodata\n", out);
                break;
            case PIR_GSEG_DATA:
                fputs(is_zero ? "\t.bss\n" : "\t.data\n", out);
                break;
            case PIR_GSEG_TLS:
                fputs(is_zero ? "\t.section .tbss,\"awT\",@nobits\n"
                              : "\t.section .tdata,\"awT\",@progbits\n", out);
                break;
        }
        return;
    }

    if (strchr(custom_sec, ',') == NULL) {
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
            if (strcmp(custom_sec, BUILTIN_SECS[i].name) == 0) {
                fputs(BUILTIN_SECS[i].dir, out);
                return;
            }
        }

        static const struct { const char *pfx; const char *type; } ARRAY_SECS[] = {
            { ".init_array",    "@init_array" },
            { ".fini_array",    "@fini_array" },
            { ".preinit_array", "@preinit_array" },
        };
        for (size_t i = 0u; i < sizeof(ARRAY_SECS) / sizeof(ARRAY_SECS[0]); ++i) {
            if (elf_sec_is_prefix(custom_sec, ARRAY_SECS[i].pfx)) {
                fputs("\t.section ", out);
                emit_elf_section_name(out, custom_sec);
                fprintf(out, ",\"aw\",%s\n", ARRAY_SECS[i].type);
                return;
            }
        }
    }

    /* Merge ELF flags/type across ALL funcs and globals sharing this custom section. */
    bool eff_exec  = is_func;
    bool eff_write = (!is_func && (seg != PIR_GSEG_RODATA || has_reloc));
    bool eff_tls   = (!is_func && seg == PIR_GSEG_TLS);
    bool eff_zero  = (!is_func && is_zero);
    if (mod) {
        for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
            const PirFunction *f = &mod->funcs.data[fi];
            if (f->is_defined && f->section && strcmp(f->section, custom_sec) == 0) {
                eff_exec = true;
                eff_zero = false;
            }
        }
        for (uint32_t gi = 0u; gi < mod->globals.len; ++gi) {
            const PirGlobal *og = &mod->globals.data[gi];
            if (og->linkage == PIR_LINK_EXTERN || !og->section ||
                strcmp(og->section, custom_sec) != 0) {
                continue;
            }
            if (!pir_global_is_all_zero(og)) eff_zero = false;
            if (og->seg == PIR_GSEG_TLS) { eff_tls = true; eff_write = true; }
            else if (og->seg != PIR_GSEG_RODATA || global_has_reloc(og)) eff_write = true;
        }
    }
    char flags[8];
    size_t fl = 0u;
    flags[fl++] = 'a';
    if (eff_write) flags[fl++] = 'w';
    if (eff_exec)  flags[fl++] = 'x';
    if (eff_tls)   flags[fl++] = 'T';
    flags[fl] = '\0';
    fputs("\t.section ", out);
    emit_elf_section_name(out, custom_sec);
    fprintf(out, ",\"%s\",%s\n", flags, eff_zero ? "@nobits" : "@progbits");
}

static void emit_macho_section_directive(FILE *out, const char *custom_sec, PirGlobalSeg seg,
                                         bool has_reloc, bool is_zero, bool is_func) {
    if (custom_sec && custom_sec[0] != '\0') {
        const char *c1 = strchr(custom_sec, ',');
        if (c1 != NULL && strchr(custom_sec, ';') == NULL) {
            if (is_func && strchr(c1 + 1, ',') == NULL) {
                fprintf(out, "\t.section %s,regular,pure_instructions\n", custom_sec);
            } else {
                fprintf(out, "\t.section %s\n", custom_sec);
            }
            return;
        }
        if (is_func) {
            fputs("\t.section __TEXT,__text,regular,pure_instructions\n", out);
            return;
        }
        /* Map ELF-style custom section names to valid Mach-O SEGMENT,section syntax. */
        if (strncmp(custom_sec, ".rodata", 7u) == 0) {
            fprintf(out, "\t.section %s\n", has_reloc ? "__DATA_CONST,__const" : "__TEXT,__const");
        } else if (strncmp(custom_sec, ".text", 5u) == 0) {
            fputs("\t.section __TEXT,__text,regular,pure_instructions\n", out);
        } else if (strncmp(custom_sec, ".bss", 4u) == 0 && is_zero) {
            fputs("\t.section __DATA,__bss\n", out);
        } else {
            fputs("\t.section __DATA,__data\n", out);
        }
        return;
    }
    if (is_func) {
        fputs("\t.section __TEXT,__text,regular,pure_instructions\n", out);
        return;
    }
    switch (seg) {
        case PIR_GSEG_RODATA:
            fprintf(out, "\t.section %s\n", has_reloc ? "__DATA_CONST,__const" : "__TEXT,__const");
            break;
        case PIR_GSEG_DATA:
            fputs("\t.section __DATA,__data\n", out);
            break;
        case PIR_GSEG_TLS:
            fputs("\t.section __DATA,__thread_data,thread_local_regular\n", out);
            break;
    }
}

void pir_asm_emit_section_directive(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                                    const char *custom_sec, PirGlobalSeg seg,
                                    bool has_reloc, bool is_zero, bool is_func) {
    switch (pir_asm_obj_format(target)) {
        case PIR_OBJ_MACHO:
            emit_macho_section_directive(out, custom_sec, seg, has_reloc, is_zero, is_func);
            break;
        case PIR_OBJ_ELF:
        case PIR_OBJ_COFF:
            emit_elf_section_directive(out, mod, custom_sec, seg, has_reloc, is_zero, is_func);
            break;
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

void pir_asm_emit_dwarf_loc(FILE *out, const PirModule *mod, const PirFunction *fn,
                            PirDwarfLocState *st, uint32_t loc_idx) {
    if (loc_idx != 0u && loc_idx != st->last_loc && loc_idx < fn->locs.len) {
        const PirSourceLoc *sl = &fn->locs.data[loc_idx];
        fprintf(out, "\t.loc %u %u %u\n",
                pir_asm_dwarf_file_num(mod, sl->file_id), sl->line, sl->col);
        st->last_loc    = loc_idx;
        st->emitted_any = true;
    } else if (!st->emitted_any && mod->files.len > 0u) {
        fputs("\t.loc 1 0 0\n", out);
        st->emitted_any = true;
    }
}

void pir_asm_emit_func_prologue(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                                const MFunc *mf, uint32_t p2align, const char *elf_type_pfx,
                                PirAsmNeedsQuoteFn arch_needs_quote) {
    const PirFunction *fn = mf->pir_fn;
    const char *lp = pir_asm_lbl_pfx(target);

    pir_asm_emit_section_directive(out, mod, target, fn->section, PIR_GSEG_RODATA, false, false, true);
    pir_asm_emit_linkage(out, target, fn->linkage, fn->name, arch_needs_quote);
    if (!pir_asm_is_macho(target)) {
        fputs("\t.type ", out);
        pir_asm_emit_sym(out, target, fn->name, NULL, arch_needs_quote);
        fprintf(out, ", %sfunction\n", elf_type_pfx);
    }
    fprintf(out, "\t.p2align %u\n", p2align);
    pir_asm_emit_sym(out, target, fn->name, NULL, arch_needs_quote);
    fputs(":\n", out);
    if (mod->files.len > 0u && pir_asm_is_macho(target)) {
        fprintf(out, "%sfunc_begin%u:\n", lp, mf->fn_index);
    }
    fputs("\t.cfi_startproc\n", out);
}

void pir_asm_emit_func_epilogue(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                                const MFunc *mf, PirAsmNeedsQuoteFn arch_needs_quote) {
    const PirFunction *fn = mf->pir_fn;
    const char *lp = pir_asm_lbl_pfx(target);

    if (mod->files.len > 0u && pir_asm_is_macho(target)) {
        fprintf(out, "%sfunc_end%u:\n", lp, mf->fn_index);
    }
    fputs("\t.cfi_endproc\n", out);
    if (!pir_asm_is_macho(target)) {
        fputs("\t.size ", out);
        pir_asm_emit_sym(out, target, fn->name, NULL, arch_needs_quote);
        fputs(", .-", out);
        pir_asm_emit_sym(out, target, fn->name, NULL, arch_needs_quote);
        fputc('\n', out);
    }
    fputc('\n', out);
}

void pir_asm_build_blk_redir(const MFunc *mf, MOpcode jmp_op,
                             uint32_t **out_redir, bool **out_skip) {
    uint32_t nblks = mf->blocks.len;
    uint32_t *blk_redir = nblks ? (uint32_t *)malloc((size_t)nblks * sizeof(uint32_t)) : NULL;
    bool     *blk_skip  = nblks ? (bool *)calloc((size_t)nblks, sizeof(bool)) : NULL;
    uint32_t *vis_mark  = nblks ? (uint32_t *)calloc((size_t)nblks, sizeof(uint32_t)) : NULL;
    if (nblks && (!blk_redir || !blk_skip || !vis_mark)) abort();

    for (uint32_t b = 0u; b < nblks; ++b) blk_redir[b] = b;
    for (uint32_t b = 1u; b < nblks; ++b) {
        const MBlock *blk = &mf->blocks.data[b];
        if (blk->insts.len == 1u && blk->insts.data[0].op == jmp_op) {
            uint32_t dst = blk->insts.data[0].ops[0].u.block_id;
            if (dst < nblks && dst != b) blk_redir[b] = dst;
        }
    }
    /* Break any empty-block cycles by pinning one canonical representative per cycle. */
    for (uint32_t b = 1u; b < nblks; ++b) {
        uint32_t cur = b;
        while (blk_redir[cur] != cur) {
            if (vis_mark[cur] == b) {
                blk_redir[cur] = cur;
                break;
            }
            if (vis_mark[cur] != 0u) break;
            vis_mark[cur] = b;
            cur = blk_redir[cur];
        }
    }
    free(vis_mark);
    /* Path-compress every block to its live root (guaranteed blk_redir[root] == root). */
    for (uint32_t b = 1u; b < nblks; ++b) {
        uint32_t cur = blk_redir[b];
        while (blk_redir[cur] != cur) cur = blk_redir[cur];
        blk_redir[b] = cur;
        if (cur != b) blk_skip[b] = true;
    }
    *out_redir = blk_redir;
    *out_skip  = blk_skip;
}

void pir_asm_emit_jtables(FILE *out, const PirTargetSpec *target, const MFunc *mf,
                          const uint32_t *blk_redir) {
    const char *lp = pir_asm_lbl_pfx(target);
    for (uint32_t j = 0u; j < mf->jtables.len; ++j) {
        const MJTable *jt = &mf->jtables.data[j];
        fputs("\t.p2align 2\n", out);
        fprintf(out, "%sJT%u_%u:\n", lp, mf->fn_index, j);
        for (uint32_t k = 0u; k < jt->num_entries; ++k) {
            uint32_t dst = blk_redir ? blk_redir[jt->targets[k]] : jt->targets[k];
            fprintf(out, "\t.long %sBB%u_%u - %sJT%u_%u\n",
                    lp, mf->fn_index, dst, lp, mf->fn_index, j);
        }
    }
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
        bool all_zero = pir_global_is_all_zero(g);
        uint64_t sz = compute_global_bytes(mod, target, g);

        /* Darwin Mach-O TLS: __DATA,__thread_vars descriptor + __thread_data / .tbss backing.
         * Use 'l_' (linker-visible private local, NOT 'L' assembler-local) so ld64
         * preserves per-variable offsets in __thread_data/__thread_bss. */
        if (pir_asm_is_macho(target) && g->seg == PIR_GSEG_TLS) {
            char tlv_init_lbl[64];
            snprintf(tlv_init_lbl, sizeof(tlv_init_lbl), "l_pir_tlv_init_%u$tlv$init", i);
            if (all_zero) {
                fprintf(out, "\t.tbss %s, %" PRIu64 ", %u\n", tlv_init_lbl, sz, p2);
            } else {
                fprintf(out, "\t.section __DATA,__thread_data,thread_local_regular\n");
                if (p2 > 0u) fprintf(out, "\t.p2align %u\n", p2);
                fprintf(out, "%s:\n", tlv_init_lbl);
                emit_global_data_items(out, mod, target, g, arch_needs_quote);
            }
            fprintf(out, "\t.section __DATA,__thread_vars,thread_local_variables\n");
            fprintf(out, "\t.p2align 3\n");
            pir_asm_emit_linkage(out, target, g->linkage, g->name, arch_needs_quote);
            pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
            fputs(":\n", out);
            fprintf(out, "\t.quad __tlv_bootstrap\n");
            fprintf(out, "\t.quad 0\n");
            fprintf(out, "\t.quad %s\n\n", tlv_init_lbl);
            continue;
        }

        /* Mach-O zeroinit mutable data in default section -> .zerofill __DATA,__bss */
        if (pir_asm_is_macho(target) && g->seg == PIR_GSEG_DATA && all_zero &&
            (!g->section || g->section[0] == '\0') &&
            (g->linkage == PIR_LINK_INTERNAL || g->linkage == PIR_LINK_EXPORT)) {
            pir_asm_emit_linkage(out, target, g->linkage, g->name, arch_needs_quote);
            fputs("\t.zerofill __DATA,__bss,", out);
            pir_asm_emit_sym(out, target, g->name, NULL, arch_needs_quote);
            fprintf(out, ",%" PRIu64 ",%u\n\n", sz, p2);
            continue;
        }

        bool has_rel = global_has_reloc(g);
        pir_asm_emit_section_directive(out, mod, target, g->section, g->seg, has_rel, all_zero, false);
        pir_asm_emit_linkage(out, target, g->linkage, g->name, arch_needs_quote);
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
