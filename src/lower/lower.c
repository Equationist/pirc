/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lower/lower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/verify.h"

/* =========================================================================
 * Shared Lowering IR Builder Helpers
 * ========================================================================= */

PirValueId pir_fn_new_val(PirModule *mod, PirFunction *fn, const char *prefix,
                          PirType ty, PirBlockId def_block, PirInstId def_inst, uint32_t slot) {
    char buf[48];
    PirValueId id = fn->values.len;
    snprintf(buf, sizeof(buf), "%s_%u", prefix ? prefix : "t", id);
    PirValueDef vd;
    vd.name        = pir_arena_strdup(&mod->arena, buf);
    vd.type        = ty;
    vd.def_block   = def_block;
    vd.def_inst    = def_inst;
    vd.param_index = slot;
    PIR_VEC_PUSH(&fn->values, vd);
    return id;
}

PirInstId pir_fn_append_inst(PirFunction *fn, PirInstIdVec *out_list, PirInst inst) {
    PirInstId id = fn->insts.len;
    if (inst.results[0] != PIR_INVALID_ID) {
        fn->values.data[inst.results[0]].def_inst = id;
    }
    if (inst.results[1] != PIR_INVALID_ID) {
        fn->values.data[inst.results[1]].def_inst = id;
    }
    PIR_VEC_PUSH(&fn->insts, inst);
    if (out_list) {
        PIR_VEC_PUSH(out_list, id);
    }
    return id;
}

PirInst pir_make_inst(PirOpcode op, PirType ty, uint32_t src_line, uint32_t loc_idx) {
    PirInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.op                = op;
    inst.ty                = ty;
    inst.aux_type          = pir_type_scalar(PIR_SCALAR_VOID);
    inst.results[0]        = PIR_INVALID_ID;
    inst.results[1]        = PIR_INVALID_ID;
    inst.sig_id            = PIR_INVALID_ID;
    inst.targets[0].target = PIR_INVALID_ID;
    inst.targets[1].target = PIR_INVALID_ID;
    inst.src_line          = src_line;
    inst.loc_idx           = loc_idx;
    return inst;
}

PirOperand pir_op_val(PirValueId id, PirType ty) {
    PirOperand o;
    memset(&o, 0, sizeof(o));
    o.kind  = PIR_OPER_VAL;
    o.type  = ty;
    o.u.val = id;
    return o;
}

PirOperand pir_op_imm_i(uint64_t val, bool is_neg, PirType ty) {
    PirOperand o;
    memset(&o, 0, sizeof(o));
    o.kind                  = PIR_OPER_IMM_INT;
    o.type                  = ty;
    o.u.imm_int.lo          = val;
    o.u.imm_int.is_negative = is_neg;
    return o;
}

PirOperand pir_op_imm_f(uint64_t bits, PirType ty) {
    PirOperand o;
    memset(&o, 0, sizeof(o));
    o.kind             = PIR_OPER_IMM_FLOAT;
    o.type             = ty;
    o.u.imm_float.bits = bits;
    return o;
}

PirOperand pir_op_sym_func(const char *name, PirFuncId id) {
    PirOperand o;
    memset(&o, 0, sizeof(o));
    o.kind          = PIR_OPER_SYM;
    o.type          = pir_type_scalar(PIR_SCALAR_PTR);
    o.u.sym.name    = name;
    o.u.sym.is_func = true;
    o.u.sym.id      = id;
    return o;
}

PirOperand *pir_alloc_ops(PirModule *mod, uint32_t n) {
    return PIR_ARENA_NEW_ARRAY(&mod->arena, PirOperand, n);
}

PirFuncId pir_ensure_extern_func(PirModule *mod, const char *name,
                                 const PirType *param_tys, uint32_t param_count,
                                 PirType ret_ty) {
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        if (strcmp(mod->funcs.data[i].name, name) == 0) {
            return i;
        }
    }
    PirFunction fn;
    memset(&fn, 0, sizeof(fn));
    fn.name        = pir_arena_strdup(&mod->arena, name);
    fn.linkage     = PIR_LINK_EXTERN;
    fn.cc          = PIR_CC_C;
    fn.attrs       = PIR_FATTR_NOUNWIND;
    fn.param_count = param_count;
    if (param_count > 0u) {
        fn.params = PIR_ARENA_NEW_ARRAY(&mod->arena, PirParamDecl, param_count);
        for (uint32_t i = 0u; i < param_count; ++i) {
            fn.params[i].type     = param_tys[i];
            fn.params[i].abi_type = pir_type_scalar(PIR_SCALAR_VOID);
            fn.params[i].val_id   = PIR_INVALID_ID;
        }
    }
    fn.ret_type   = ret_ty;
    fn.is_defined = false;
    PirFuncId id  = mod->funcs.len;
    PIR_VEC_PUSH(&mod->funcs, fn);
    return id;
}

static void replace_in_ops(PirOperand *ops, uint32_t n, PirValueId old_v, PirValueId new_v) {
    for (uint32_t i = 0u; i < n; ++i) {
        if (ops[i].kind == PIR_OPER_VAL && ops[i].u.val == old_v) {
            ops[i].u.val = new_v;
        }
    }
}

void pir_fn_replace_val_uses(PirFunction *fn, PirValueId old_v, PirValueId new_v) {
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            PirInst *inst = &fn->insts.data[blk->insts.data[j]];
            replace_in_ops(inst->operands, inst->operand_count, old_v, new_v);
            replace_in_ops(inst->targets[0].args, inst->targets[0].arg_count, old_v, new_v);
            replace_in_ops(inst->targets[1].args, inst->targets[1].arg_count, old_v, new_v);
            for (uint32_t c = 0u; c < inst->switch_case_count; ++c) {
                replace_in_ops(inst->switch_cases[c].target.args,
                               inst->switch_cases[c].target.arg_count, old_v, new_v);
            }
        }
    }
}

/* =========================================================================
 * Master Lowering Orchestrator (Strict Order per PIR.md §6.2)
 * ========================================================================= */

static bool maybe_verify_hir(const PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
#ifdef PIR_VERIFY_ALL
    return pir_verify_module(mod, PIR_VERIFY_HIR, target, diag);
#else
    (void)mod; (void)target; (void)diag;
    return true;
#endif
}

bool pir_lower_module(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    if (!pir_lower_exceptions(mod, target, diag)) return false;
    if (!maybe_verify_hir(mod, target, diag))     return false;

    if (!pir_lower_varargs(mod, target, diag))    return false;
    if (!maybe_verify_hir(mod, target, diag))     return false;

    if (!pir_lower_cabi(mod, target, diag))       return false;
    if (!maybe_verify_hir(mod, target, diag))     return false;

    if (!pir_lower_aggregates(mod, target, diag)) return false;
    if (!maybe_verify_hir(mod, target, diag))     return false;

    if (!pir_legalize_i128(mod, target, diag))    return false;
    if (!maybe_verify_hir(mod, target, diag))     return false;

    if (!pir_legalize_simd(mod, target, diag))    return false;
    if (!maybe_verify_hir(mod, target, diag))     return false;

    if (!pir_legalize_optional_ops(mod, target, diag)) return false;

    /* Final LIR Verification Gate (always runs). */
    return pir_verify_module(mod, PIR_VERIFY_LIR, target, diag);
}
