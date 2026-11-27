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

bool        pir_asm_is_macho(const PirTargetSpec *target);
const char *pir_asm_sym_pfx(const PirTargetSpec *target);
const char *pir_asm_lbl_pfx(const PirTargetSpec *target);
uint32_t    pir_asm_log2_align(uint32_t align);
MCond       pir_asm_invert_cond(MCond c);

/* Emit a properly escaped double-quoted C/gas string literal (including quotes). */
void pir_asm_emit_escaped_cstr(FILE *out, const char *s);

/* Stream an assembly symbol (with target prefix and optional suffix), quoting if
 * required by syntax, leading digits/dot/dollar, or arch_needs_quote. */
void pir_asm_emit_sym(FILE *out, const PirTargetSpec *target,
                      const char *name, const char *suffix,
                      PirAsmNeedsQuoteFn arch_needs_quote);

/* Emit section directive for functions or globals (Mach-O & ELF). */
void pir_asm_emit_section_directive(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                                    const char *custom_sec, PirGlobalSeg seg,
                                    bool has_reloc, bool is_zero, bool is_func);

/* Emit DWARF .file directives for the module, and map PirFileId to 1-based DWARF index. */
void     pir_asm_emit_dwarf_files(FILE *out, const PirModule *mod);
uint32_t pir_asm_dwarf_file_num(const PirModule *mod, PirFileId fid);

/* Emit all non-extern module globals (data, rodata, bss, TLS). */
bool pir_asm_emit_globals(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                          PirAsmNeedsQuoteFn arch_needs_quote);

/* Emit Mach-O DWARF CU footer + .subsections_via_symbols, or ELF .note.GNU-stack. */
bool pir_asm_emit_module_footer(FILE *out, const PirModule *mod, const PirTargetSpec *target);

#endif /* PIR_BACKEND_ASM_COMMON_H */
