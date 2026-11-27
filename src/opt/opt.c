/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "opt/opt.h"

#include <string.h>

#include "ir/verify.h"

bool pir_op_is_tls_sym(const PirModule *mod, const PirOperand *op) {
    if (!mod || !op || op->kind != PIR_OPER_SYM || op->u.sym.is_func || !op->u.sym.name) {
        return false;
    }
    if (op->u.sym.id < mod->globals.len) {
        const PirGlobal *g = &mod->globals.data[op->u.sym.id];
        if (g->name && strcmp(g->name, op->u.sym.name) == 0) {
            return g->seg == PIR_GSEG_TLS;
        }
    }
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        if (g->name && strcmp(g->name, op->u.sym.name) == 0) {
            return g->seg == PIR_GSEG_TLS;
        }
    }
    return false;
}

uint32_t pir_inst_effects(const PirModule *mod, const PirFunction *fn, const PirInst *ins) {
    if (!ins) return PIR_EFF_NONE;
    uint32_t eff = PIR_EFF_NONE;

    uint32_t trap_flags = PIR_FLAG_TRAP | PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U;
    uint32_t ub_flags   = trap_flags | PIR_FLAG_UNSAFE | PIR_FLAG_UNSAFE_S |
                          PIR_FLAG_UNSAFE_U | PIR_FLAG_NONZERO;
    if ((ins->flags & trap_flags) != 0u) eff |= PIR_EFF_MAY_TRAP;
    if ((ins->flags & ub_flags) != 0u)   eff |= PIR_EFF_HAS_UB_FLAG;

    bool pure_flags_ok = ((ins->flags & (trap_flags | PIR_FLAG_UNSAFE |
                                         PIR_FLAG_NONZERO | PIR_FLAG_VOLATILE)) == 0u);
    if (pure_flags_ok && (ins->flags & (PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) != 0u &&
        ins->op != PIR_OP_ADD && ins->op != PIR_OP_SUB && ins->op != PIR_OP_MUL) {
        pure_flags_ok = false;
    }
    /* Any instruction referencing a TLS symbol operand materializes a TLV thunk call,
     * which may trap (e.g. undefined weak extern TLS) and clobbers caller-saved state. */
    for (uint32_t i = 0u; i < ins->operand_count; ++i) {
        if (pir_op_is_tls_sym(mod, &ins->operands[i])) {
            eff |= PIR_EFF_MAY_TRAP | PIR_EFF_REAL_CALL;
            pure_flags_ok = false;
            break;
        }
    }

    switch (ins->op) {
        case PIR_OP_CONST:
        case PIR_OP_ADDR:
            if (pure_flags_ok) {
                eff |= PIR_EFF_PURE_HOISTABLE;
            }
            break;

        case PIR_OP_ADD: case PIR_OP_SUB: case PIR_OP_MUL:
        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
        case PIR_OP_MUL_WIDE_U: case PIR_OP_MUL_WIDE_S:
        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
        case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR:
        case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR:
        case PIR_OP_CLZ: case PIR_OP_CTZ: case PIR_OP_POPCNT: case PIR_OP_BSWAP:
        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV: case PIR_OP_FNEG:
        case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_S: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_S: case PIR_OP_CMP_GE_U:
        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
        case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
        case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE:
        case PIR_OP_ZEXT: case PIR_OP_SEXT: case PIR_OP_TRUNC:
        case PIR_OP_SITOFP: case PIR_OP_UITOFP: case PIR_OP_FPEXT: case PIR_OP_FPTRUNC:
        case PIR_OP_BITCAST: case PIR_OP_PTRTOINT: case PIR_OP_INTTOPTR:
        case PIR_OP_PTR_ADD: case PIR_OP_SPLAT: case PIR_OP_EXTRACT:
        case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
            if (pure_flags_ok) eff |= PIR_EFF_PURE_HOISTABLE;
            break;

        case PIR_OP_SELECT:
            if (pure_flags_ok && ins->ty.kind == PIR_TYPE_SCALAR && ins->operand_count == 3u &&
                pir_operand_is_proven_bool_i8(fn, ins->operands[0])) {
                eff |= PIR_EFF_PURE_HOISTABLE;
            }
            break;

        case PIR_OP_FPTOSI:
        case PIR_OP_FPTOUI:
            if ((ins->flags & PIR_FLAG_SAT) != 0u) {
                if (pure_flags_ok) eff |= PIR_EFF_PURE_HOISTABLE;
            } else {
                eff |= PIR_EFF_MAY_TRAP;
            }
            break;

        case PIR_OP_DIV_S:
        case PIR_OP_DIV_U:
        case PIR_OP_REM_S:
        case PIR_OP_REM_U:
            if ((ins->flags & PIR_FLAG_UNSAFE) == 0u) eff |= PIR_EFF_MAY_TRAP;
            break;

        case PIR_OP_ALLOCA:
        case PIR_OP_ALLOCA_PTR:
        case PIR_OP_STACKSAVE:
        case PIR_OP_FIELD_PTR:
        case PIR_OP_ELEM_PTR:
            break;

        case PIR_OP_ALLOCA_DYN:
            eff |= PIR_EFF_MAY_WRITE_MEM;
            break;

        case PIR_OP_STACKRESTORE:
            eff |= PIR_EFF_MAY_TRAP | PIR_EFF_MAY_WRITE_MEM | PIR_EFF_MEM_BARRIER;
            break;

        case PIR_OP_LOAD:
            eff |= PIR_EFF_MAY_READ_MEM;
            if ((ins->flags & PIR_FLAG_VOLATILE) != 0u) {
                eff |= PIR_EFF_MAY_TRAP | PIR_EFF_MEM_BARRIER;
            }
            break;

        case PIR_OP_STORE:
            eff |= PIR_EFF_MAY_WRITE_MEM;
            if ((ins->flags & PIR_FLAG_VOLATILE) != 0u) {
                eff |= PIR_EFF_MAY_TRAP | PIR_EFF_MEM_BARRIER;
            }
            break;

        case PIR_OP_MEMCPY:
        case PIR_OP_MEMMOVE:
            eff |= PIR_EFF_MAY_READ_MEM | PIR_EFF_MAY_WRITE_MEM | PIR_EFF_MEM_BARRIER;
            break;

        case PIR_OP_MEMSET:
            eff |= PIR_EFF_MAY_WRITE_MEM | PIR_EFF_MEM_BARRIER;
            break;

        case PIR_OP_ATOMIC_LOAD:
            eff |= PIR_EFF_MAY_TRAP | PIR_EFF_MAY_READ_MEM | PIR_EFF_MEM_BARRIER;
            break;

        case PIR_OP_ATOMIC_STORE:
            eff |= PIR_EFF_MAY_TRAP | PIR_EFF_MAY_WRITE_MEM | PIR_EFF_MEM_BARRIER;
            break;

        case PIR_OP_ATOMIC_RMW:
        case PIR_OP_ATOMIC_CMPXCHG:
        case PIR_OP_FENCE:
            eff |= PIR_EFF_MAY_TRAP | PIR_EFF_MAY_READ_MEM | PIR_EFF_MAY_WRITE_MEM |
                   PIR_EFF_MEM_BARRIER;
            break;

        case PIR_OP_VA_START:
        case PIR_OP_VA_END:
            eff |= PIR_EFF_MAY_WRITE_MEM | PIR_EFF_MEM_BARRIER;
            break;

        case PIR_OP_VA_ARG:
        case PIR_OP_VA_ARG_BUF:
        case PIR_OP_VA_COPY:
            eff |= PIR_EFF_MAY_READ_MEM | PIR_EFF_MAY_WRITE_MEM | PIR_EFF_MEM_BARRIER;
            break;

        case PIR_OP_CALL:
            if (!pir_inst_is_pure_math_call(mod, fn, ins)) {
                eff |= PIR_EFF_MAY_TRAP | PIR_EFF_MAY_READ_MEM | PIR_EFF_MAY_WRITE_MEM |
                       PIR_EFF_MEM_BARRIER | PIR_EFF_REAL_CALL;
            }
            break;

        case PIR_OP_CALL_INDIRECT:
            eff |= PIR_EFF_MAY_TRAP | PIR_EFF_MAY_READ_MEM | PIR_EFF_MAY_WRITE_MEM |
                   PIR_EFF_MEM_BARRIER | PIR_EFF_REAL_CALL;
            break;

        case PIR_OP_INVOKE:
        case PIR_OP_INVOKE_INDIRECT:
            eff |= PIR_EFF_CONTROL_FLOW | PIR_EFF_MAY_TRAP | PIR_EFF_MAY_READ_MEM |
                   PIR_EFF_MAY_WRITE_MEM | PIR_EFF_MEM_BARRIER | PIR_EFF_REAL_CALL;
            break;

        case PIR_OP_RESUME:
            eff |= PIR_EFF_CONTROL_FLOW | PIR_EFF_MAY_TRAP | PIR_EFF_MAY_READ_MEM |
                   PIR_EFF_MAY_WRITE_MEM | PIR_EFF_MEM_BARRIER;
            break;

        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE:
            eff |= PIR_EFF_CONTROL_FLOW | PIR_EFF_MAY_TRAP;
            break;

        case PIR_OP_JMP:
        case PIR_OP_BR:
        case PIR_OP_SWITCH:
        case PIR_OP_RET:
            eff |= PIR_EFF_CONTROL_FLOW;
            break;
    }
    return eff;
}

bool pir_inst_is_pure_math_call(const PirModule *mod, const PirFunction *fn, const PirInst *ins) {
    if (!mod || !fn || !ins || ins->op != PIR_OP_CALL || ins->operand_count != 2u ||
        ins->results[0] == PIR_INVALID_ID || ins->results[1] != PIR_INVALID_ID ||
        ins->operands[0].kind != PIR_OPER_SYM || !ins->operands[0].u.sym.name ||
        !ins->operands[0].u.sym.is_func || ins->operands[0].u.sym.id >= mod->funcs.len) {
        return false;
    }
    const PirFunction *callee = &mod->funcs.data[ins->operands[0].u.sym.id];
    if (callee->is_defined || callee->is_variadic || callee->param_count != 1u ||
        callee->cabi_ret.kind != PIR_CABI_RET_NONE) {
        return false;
    }
    const char *cn = ins->operands[0].u.sym.name;
    PirType expect;
    if (strcmp(cn, "sqrt") == 0 || strcmp(cn, "fabs") == 0) {
        expect = pir_type_scalar(PIR_SCALAR_F64);
    } else if (strcmp(cn, "sqrtf") == 0 || strcmp(cn, "fabsf") == 0) {
        expect = pir_type_scalar(PIR_SCALAR_F32);
    } else {
        return false;
    }
    if (!pir_type_eq(callee->ret_type, expect) || !pir_type_eq(callee->params[0].type, expect)) {
        return false;
    }
    if (ins->results[0] >= fn->values.len || !pir_type_eq(fn->values.data[ins->results[0]].type, expect)) {
        return false;
    }
    PirType arg_ty = ins->operands[1].type;
    if (ins->operands[1].kind == PIR_OPER_VAL) {
        if (ins->operands[1].u.val >= fn->values.len) return false;
        arg_ty = fn->values.data[ins->operands[1].u.val].type;
    }
    return pir_type_eq(arg_ty, expect);
}

bool pir_is_returns_twice_name(const char *cn) {
    if (!cn) return false;
    return strcmp(cn, "__pir_setjmp") == 0 || strcmp(cn, "setjmp") == 0 ||
           strcmp(cn, "_setjmp") == 0 || strcmp(cn, "sigsetjmp") == 0 ||
           strcmp(cn, "__sigsetjmp") == 0 || strcmp(cn, "__setjmp") == 0 ||
           strcmp(cn, "vfork") == 0 || strcmp(cn, "getcontext") == 0 ||
           strcmp(cn, "savectx") == 0;
}

bool pir_fn_has_returns_twice(const PirFunction *fn) {
    if (!fn) return false;
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_CALL && ins->operand_count >= 1u &&
                ins->operands[0].kind == PIR_OPER_SYM &&
                pir_is_returns_twice_name(ins->operands[0].u.sym.name)) {
                return true;
            }
        }
    }
    return false;
}

bool pir_operand_is_proven_bool_i8(const PirFunction *fn, PirOperand op) {
    if (op.kind == PIR_OPER_IMM_INT) {
        uint64_t v = op.u.imm_int.is_negative ? (UINT64_C(0) - op.u.imm_int.lo) : op.u.imm_int.lo;
        return (v & 0xffu) <= 1u;
    }
    if (op.kind == PIR_OPER_VAL && fn && op.u.val < fn->values.len) {
        PirInstId did = fn->values.data[op.u.val].def_inst;
        if (did == PIR_INVALID_ID || did >= fn->insts.len) return false;
        const PirInst *d = &fn->insts.data[did];
        if ((d->op >= PIR_OP_CMP_EQ && d->op <= PIR_OP_CMP_GE_U) ||
            (d->op >= PIR_OP_FCMP_OEQ && d->op <= PIR_OP_FCMP_UGE)) {
            return d->ty.kind == PIR_TYPE_SCALAR;
        }
        if ((d->op >= PIR_OP_ADD_OVF_S && d->op <= PIR_OP_MUL_OVF_U) ||
            d->op == PIR_OP_ADD_CARRY || d->op == PIR_OP_SUB_BORROW) {
            return d->results[1] == op.u.val;
        }
        if (d->op == PIR_OP_CONST && d->operand_count == 1u &&
            d->operands[0].kind == PIR_OPER_IMM_INT) {
            uint64_t v = d->operands[0].u.imm_int.is_negative
                         ? (UINT64_C(0) - d->operands[0].u.imm_int.lo)
                         : d->operands[0].u.imm_int.lo;
            return (v & 0xffu) <= 1u;
        }
    }
    return false;
}

bool pir_operand_eq_exact(PirOperand a, PirOperand b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case PIR_OPER_VAL:
            return a.u.val == b.u.val;
        case PIR_OPER_IMM_INT:
            return pir_type_eq(a.type, b.type) &&
                   a.u.imm_int.lo == b.u.imm_int.lo &&
                   a.u.imm_int.hi == b.u.imm_int.hi &&
                   a.u.imm_int.is_negative == b.u.imm_int.is_negative;
        case PIR_OPER_IMM_FLOAT:
            return pir_type_eq(a.type, b.type) &&
                   a.u.imm_float.bits == b.u.imm_float.bits;
        case PIR_OPER_SYM:
            return a.u.sym.is_func == b.u.sym.is_func &&
                   a.u.sym.offset == b.u.sym.offset &&
                   a.u.sym.name && b.u.sym.name &&
                   strcmp(a.u.sym.name, b.u.sym.name) == 0;
        case PIR_OPER_INVOKE_RET:
        case PIR_OPER_INVOKE_EXN:
            return true;
    }
    return false;
}

static bool verify_after_pass(const PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
#ifdef PIR_VERIFY_ALL
    return pir_verify_module(mod, PIR_VERIFY_LIR, target, diag);
#else
    (void)mod; (void)target; (void)diag;
    return true;
#endif
}

typedef void (*FnPassSimple)(PirModule *, PirFunction *);
typedef void (*FnPassTarget)(PirModule *, PirFunction *, const PirTargetSpec *);

static bool run_simple(PirModule *mod, const PirTargetSpec *target, PirDiag *diag, FnPassSimple p) {
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        if (mod->funcs.data[fi].is_defined) {
            p(mod, &mod->funcs.data[fi]);
        }
    }
    return verify_after_pass(mod, target, diag);
}

static bool run_target(PirModule *mod, const PirTargetSpec *target, PirDiag *diag, FnPassTarget p) {
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        if (mod->funcs.data[fi].is_defined) {
            p(mod, &mod->funcs.data[fi], target);
        }
    }
    return verify_after_pass(mod, target, diag);
}

bool pir_opt_module(PirModule *mod, const PirTargetSpec *target, int opt_level, PirDiag *diag) {
    if (opt_level <= 0) {
        return true;
    }

    if (!run_simple(mod, target, diag, pir_opt_simplify_cfg)) return false;
    if (!run_target(mod, target, diag, pir_opt_mem2reg))      return false;
    if (!run_target(mod, target, diag, pir_opt_gvn))          return false;

    if (opt_level >= 2) {
        if (!run_simple(mod, target, diag, pir_opt_dce))          return false;
        if (!run_simple(mod, target, diag, pir_opt_simplify_cfg)) return false;
        pir_opt_inline(mod, target);
        if (!verify_after_pass(mod, target, diag))                return false;
        if (!run_simple(mod, target, diag, pir_opt_simplify_cfg)) return false;
        if (!run_target(mod, target, diag, pir_opt_mem2reg))      return false;
        if (!run_target(mod, target, diag, pir_opt_gvn))          return false;
        if (!run_target(mod, target, diag, pir_opt_licm))         return false;
        if (!run_target(mod, target, diag, pir_opt_dse))          return false;
        if (!run_target(mod, target, diag, pir_opt_gvn))          return false;
    }

    if (!run_simple(mod, target, diag, pir_opt_dce))          return false;
    if (!run_simple(mod, target, diag, pir_opt_simplify_cfg)) return false;
    if (opt_level >= 2) {
        if (!run_target(mod, target, diag, pir_opt_loop_vec))    return false;
        if (!run_simple(mod, target, diag, pir_opt_dce))         return false;
        if (!run_simple(mod, target, diag, pir_opt_loop_rotate)) return false;
        if (!run_target(mod, target, diag, pir_opt_licm))        return false;
        if (!run_target(mod, target, diag, pir_opt_dse))         return false;
        if (!run_simple(mod, target, diag, pir_opt_dce))         return false;
    }
    if (!run_simple(mod, target, diag, pir_opt_edge_split))   return false;

    return pir_verify_module(mod, PIR_VERIFY_LIR, target, diag);
}
