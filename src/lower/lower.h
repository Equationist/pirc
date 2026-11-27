/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_LOWER_LOWER_H
#define PIR_LOWER_LOWER_H

#include <stdbool.h>
#include <stdint.h>

#include "common/diag.h"
#include "ir/ir.h"

typedef struct {
    uint64_t size;
    uint32_t align;
    uint64_t stride;   /* For arrays: element stride */
    bool     overflow; /* True if size/offset calculation overflowed 64 bits */
} PirTypeLayout;

/* Master HIR -> LIR lowering pipeline (verify-after-every-pass in PIR_VERIFY_ALL) */
bool pir_lower_module(PirModule *mod, const PirTargetSpec *target, PirDiag *diag);

/* Individual M3/M4 lowering passes (strict order per PIR.md §6.2) */
bool pir_lower_exceptions(PirModule *mod, const PirTargetSpec *target, PirDiag *diag);
bool pir_lower_varargs(PirModule *mod, const PirTargetSpec *target, PirDiag *diag);
bool pir_lower_cabi(PirModule *mod, const PirTargetSpec *target, PirDiag *diag);
bool pir_lower_aggregates(PirModule *mod, const PirTargetSpec *target, PirDiag *diag);
bool pir_legalize_i128(PirModule *mod, const PirTargetSpec *target, PirDiag *diag);
bool pir_legalize_simd(PirModule *mod, const PirTargetSpec *target, PirDiag *diag);
bool pir_legalize_optional_ops(PirModule *mod, const PirTargetSpec *target, PirDiag *diag);

/* Layout & aggregate property queries (single source of truth across verifier, lowering, ABI) */
PirTypeLayout pir_type_layout(const PirModule *mod, const PirTargetSpec *target, PirType ty);
uint64_t      pir_field_offset(const PirModule *mod, const PirTargetSpec *target,
                               PirTypeId agg_id, uint32_t field_idx);
bool          pir_agg_is_pad_only_cached(const PirModule *mod, PirType ty, int8_t *cache);
bool          pir_agg_is_pad_only(const PirModule *mod, PirType ty);

/* Shared IR builder helpers for lowering & optimization passes */
PirValueId pir_fn_new_val(PirModule *mod, PirFunction *fn, const char *prefix,
                          PirType ty, PirBlockId def_block, PirInstId def_inst, uint32_t slot);
PirBlockId pir_fn_new_block(PirModule *mod, PirFunction *fn, const char *prefix,
                            uint32_t src_line);
PirInstId  pir_fn_append_inst(PirFunction *fn, PirInstIdVec *out_list, PirInst inst);
PirInst    pir_make_inst(PirOpcode op, PirType ty, uint32_t src_line, uint32_t loc_idx);
PirOperand pir_op_val(PirValueId id, PirType ty);
PirOperand pir_op_imm_i(uint64_t val, bool is_neg, PirType ty);
PirOperand pir_op_imm_f(uint64_t bits, PirType ty);
PirOperand pir_op_sym_func(const char *name, PirFuncId id);
void       pir_op_strip_call_meta(PirOperand *op);
PirOperand *pir_alloc_ops(PirModule *mod, uint32_t n);
PirFuncId  pir_ensure_extern_func(PirModule *mod, const char *name,
                                  const PirType *param_tys, uint32_t param_count,
                                  PirType ret_ty);
PirFuncId  pir_ensure_verified_extern_func(PirModule *mod, const char *name,
                                           const PirType *param_tys, uint32_t param_count,
                                           PirType ret_ty, uint32_t src_line,
                                           const char *pass_ctx, PirDiag *diag);
void       pir_fn_replace_val_uses(PirFunction *fn, PirValueId old_v, PirValueId new_v);
PirValueId pir_emit_ptr_add_const(PirModule *mod, PirFunction *fn, PirBlockId b,
                                  PirInstIdVec *out, PirValueId base, uint64_t off,
                                  const PirTargetSpec *target, uint32_t ln, uint32_t lc);
PirValueId pir_emit_align_ptr(PirModule *mod, PirFunction *fn, PirBlockId b,
                              PirInstIdVec *out, PirValueId ptr_val, uint64_t align,
                              const PirTargetSpec *target, uint32_t ln, uint32_t lc);

#endif /* PIR_LOWER_LOWER_H */
