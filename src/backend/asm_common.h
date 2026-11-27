/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_BACKEND_ASM_COMMON_H
#define PIR_BACKEND_ASM_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "backend/mir.h"
#include "ir/ir.h"

typedef bool (*PirAsmNeedsQuoteFn)(const char *name);

typedef enum {
    PIR_OBJ_ELF = 0,
    PIR_OBJ_MACHO,
    PIR_OBJ_COFF
} PirObjFormat;

typedef struct {
    uint32_t last_loc;
    bool     emitted_any;
} PirDwarfLocState;

PirObjFormat pir_asm_obj_format(const PirTargetSpec *target);
bool         pir_asm_is_macho(const PirTargetSpec *target);
const char  *pir_asm_sym_pfx(const PirTargetSpec *target);
const char  *pir_asm_lbl_pfx(const PirTargetSpec *target);
uint32_t     pir_asm_log2_align(uint32_t align);
MCond        pir_asm_invert_cond(MCond c);

/* Emit a properly escaped double-quoted C/gas string literal (including quotes). */
void pir_asm_emit_escaped_cstr(FILE *out, const char *s);

/* Stream an assembly symbol (with target prefix and optional suffix), quoting if
 * required by syntax, leading digits/dot/dollar, or arch_needs_quote. */
void pir_asm_emit_sym(FILE *out, const PirTargetSpec *target,
                      const char *name, const char *suffix,
                      PirAsmNeedsQuoteFn arch_needs_quote);

/* Emit linkage directives (.globl, .weak_definition / .weak) for a defined symbol. */
void pir_asm_emit_linkage(FILE *out, const PirTargetSpec *target,
                          PirLinkage linkage, const char *name,
                          PirAsmNeedsQuoteFn arch_needs_quote);

/* Emit section directive for functions or globals (Mach-O & ELF). */
void pir_asm_emit_section_directive(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                                    const char *custom_sec, PirGlobalSeg seg,
                                    bool has_reloc, bool is_zero, bool is_func);

/* Emit DWARF .file directives, map PirFileId to 1-based DWARF index, and emit deduped .loc. */
void     pir_asm_emit_dwarf_files(FILE *out, const PirModule *mod);
uint32_t pir_asm_dwarf_file_num(const PirModule *mod, PirFileId fid);
void     pir_asm_emit_dwarf_loc(FILE *out, const PirModule *mod, const PirFunction *fn,
                                PirDwarfLocState *st, uint32_t loc_idx);

/* Emit shared function header/footer directives (.section, linkage, .type/.size, CFI, labels). */
void pir_asm_emit_func_prologue(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                                const MFunc *mf, uint32_t p2align, const char *elf_type_pfx,
                                PirAsmNeedsQuoteFn arch_needs_quote);
void pir_asm_emit_func_epilogue(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                                const MFunc *mf, PirAsmNeedsQuoteFn arch_needs_quote);

/* Build cycle-safe, path-compressed empty trampoline block redirection tables. */
void pir_asm_build_blk_redir(const MFunc *mf, MOpcode jmp_op,
                             uint32_t **out_redir, bool **out_skip);

/* Emit PC-relative switch jump tables following a function body. */
void pir_asm_emit_jtables(FILE *out, const PirTargetSpec *target, const MFunc *mf,
                          const uint32_t *blk_redir);

/* Emit all non-extern module globals (data, rodata, bss, TLS). */
bool pir_asm_emit_globals(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                          PirAsmNeedsQuoteFn arch_needs_quote);

/* Emit Mach-O DWARF CU footer + .subsections_via_symbols, or ELF .note.GNU-stack. */
bool pir_asm_emit_module_footer(FILE *out, const PirModule *mod, const PirTargetSpec *target);

#endif /* PIR_BACKEND_ASM_COMMON_H */
