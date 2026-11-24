/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ir/verify.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/cfg.h"
#include "analysis/dom.h"
#include "target/target.h"

typedef struct {
    const PirModule     *mod;
    PirVerifyMode        mode;
    const PirTargetSpec *target;
    PirDiag             *diag;
} Verifier;

static const char *type_str(const PirModule *mod, PirType ty, char *buf, size_t n) {
    switch (ty.kind) {
        case PIR_TYPE_SCALAR:
            switch (ty.u.scalar) {
                case PIR_SCALAR_VOID:   return "void";
                case PIR_SCALAR_I8:     return "i8";
                case PIR_SCALAR_I16:    return "i16";
                case PIR_SCALAR_I32:    return "i32";
                case PIR_SCALAR_I64:    return "i64";
                case PIR_SCALAR_I128:   return "i128";
                case PIR_SCALAR_F32:    return "f32";
                case PIR_SCALAR_F64:    return "f64";
                case PIR_SCALAR_PTR:    return "ptr";
                case PIR_SCALAR_VALIST: return "va_list";
            }
            return "void";
        case PIR_TYPE_VECTOR:
            switch (ty.u.vec) {
                case PIR_VEC_NONE:   return "none";
                case PIR_VEC_V8I8:   return "v8i8";
                case PIR_VEC_V4I16:  return "v4i16";
                case PIR_VEC_V2I32:  return "v2i32";
                case PIR_VEC_V2F32:  return "v2f32";
                case PIR_VEC_V16I8:  return "v16i8";
                case PIR_VEC_V8I16:  return "v8i16";
                case PIR_VEC_V4I32:  return "v4i32";
                case PIR_VEC_V2I64:  return "v2i64";
                case PIR_VEC_V4F32:  return "v4f32";
                case PIR_VEC_V2F64:  return "v2f64";
                case PIR_VEC_V32I8:  return "v32i8";
                case PIR_VEC_V16I16: return "v16i16";
                case PIR_VEC_V8I32:  return "v8i32";
                case PIR_VEC_V4I64:  return "v4i64";
                case PIR_VEC_V8F32:  return "v8f32";
                case PIR_VEC_V4F64:  return "v4f64";
            }
            return "none";
        case PIR_TYPE_NAMED_AGG:
            if (ty.u.agg_id < mod->agg_types.len && mod->agg_types.data[ty.u.agg_id].name) {
                snprintf(buf, n, "@%s", mod->agg_types.data[ty.u.agg_id].name);
            } else {
                snprintf(buf, n, "[array]");
            }
            return buf;
    }
    return "?";
}

static bool has_symbol(const PirModule *mod, const char *name) {
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        if (strcmp(mod->globals.data[i].name, name) == 0) return true;
    }
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        if (strcmp(mod->funcs.data[i].name, name) == 0) return true;
    }
    return false;
}

static bool check_lir_type(Verifier *v, PirType ty, uint32_t line, const char *ctx) {
    if (v->mode != PIR_VERIFY_LIR) return true;
    switch (ty.kind) {
        case PIR_TYPE_SCALAR:
            if (ty.u.scalar == PIR_SCALAR_I128) {
                pir_diag_error(v->diag, line, 0u, "LIR forbids 'i128' in %s", ctx);
                return false;
            }
            if (ty.u.scalar == PIR_SCALAR_VALIST) {
                pir_diag_error(v->diag, line, 0u, "LIR forbids 'va_list' in %s", ctx);
                return false;
            }
            return true;
        case PIR_TYPE_VECTOR:
            if (v->target && pir_vec_bits(ty.u.vec) > v->target->caps.max_simd_bits) {
                pir_diag_error(v->diag, line, 0u,
                               "LIR vector width %u exceeds target max_simd_bits %u in %s",
                               (unsigned)pir_vec_bits(ty.u.vec),
                               (unsigned)v->target->caps.max_simd_bits, ctx);
                return false;
            }
            return true;
        case PIR_TYPE_NAMED_AGG:
            pir_diag_error(v->diag, line, 0u, "LIR forbids aggregate type in %s", ctx);
            return false;
    }
    return true;
}

static bool check_ssa_type(Verifier *v, PirType ty, uint32_t line, const char *ctx) {
    if (!pir_type_is_ssa(ty)) {
        char b[64];
        pir_diag_error(v->diag, line, 0u, "type '%s' is not valid in SSA (%s)",
                       type_str(v->mod, ty, b, sizeof(b)), ctx);
        return false;
    }
    return check_lir_type(v, ty, line, ctx);
}

static bool check_agg_cycles(Verifier *v, uint32_t id, uint8_t *color) {
    if (color[id] == 1u) {
        pir_diag_error(v->diag, v->mod->agg_types.data[id].src_line, 0u,
                       "recursive/cyclic aggregate type '@%s'",
                       v->mod->agg_types.data[id].name ? v->mod->agg_types.data[id].name : "anon");
        return false;
    }
    if (color[id] == 2u) return true;
    color[id] = 1u;
    const PirAggDef *a = &v->mod->agg_types.data[id];
    if (a->kind == PIR_AGG_ARRAY) {
        if (a->elem_type.kind == PIR_TYPE_NAMED_AGG) {
            if (!check_agg_cycles(v, a->elem_type.u.agg_id, color)) return false;
        }
    } else {
        for (uint32_t i = 0u; i < a->field_count; ++i) {
            if (a->fields[i].kind == PIR_TYPE_NAMED_AGG) {
                if (!check_agg_cycles(v, a->fields[i].u.agg_id, color)) return false;
            }
        }
    }
    color[id] = 2u;
    return true;
}

static void verify_params_and_ret(Verifier *v, const char *owner, uint32_t line,
                                  PirCallingConv cc, const PirParamDecl *params,
                                  uint32_t param_count, bool is_variadic, PirType ret_type) {
    if (is_variadic && cc != PIR_CC_C) {
        pir_diag_error(v->diag, line, 0u, "'%s': variadic '...' requires cc \"c\"", owner);
    }
    if (!(ret_type.kind == PIR_TYPE_SCALAR && ret_type.u.scalar == PIR_SCALAR_VOID)) {
        check_ssa_type(v, ret_type, line, "return type");
    }
    for (uint32_t i = 0u; i < param_count; ++i) {
        const PirParamDecl *pd = &params[i];
        if (pd->abi_kind != PIR_PARAM_NORMAL) {
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, line, 0u, "LIR forbids byval/retbuf parameter on '%s'", owner);
            }
            if (cc != PIR_CC_C) {
                pir_diag_error(v->diag, line, 0u, "'%s': byval/retbuf requires cc \"c\"", owner);
            }
            if (pd->abi_kind == PIR_PARAM_RETBUF) {
                if (i != 0u) {
                    pir_diag_error(v->diag, line, 0u, "'%s': retbuf must be parameter 0", owner);
                }
                if (!(ret_type.kind == PIR_TYPE_SCALAR && ret_type.u.scalar == PIR_SCALAR_VOID)) {
                    pir_diag_error(v->diag, line, 0u, "'%s': retbuf function must return void", owner);
                }
                if (pd->abi_type.kind != PIR_TYPE_NAMED_AGG) {
                    pir_diag_error(v->diag, line, 0u, "'%s': retbuf requires aggregate type", owner);
                }
            } else { /* BYVAL */
                bool ok = (pd->abi_type.kind == PIR_TYPE_NAMED_AGG) ||
                          (pd->abi_type.kind == PIR_TYPE_SCALAR &&
                           pd->abi_type.u.scalar == PIR_SCALAR_VALIST);
                if (!ok) {
                    pir_diag_error(v->diag, line, 0u, "'%s': byval requires aggregate or va_list", owner);
                }
            }
        } else {
            check_ssa_type(v, pd->type, line, "parameter");
        }
        if (pd->attrs != PIR_PATTR_NONE &&
            !(pd->type.kind == PIR_TYPE_SCALAR && pd->type.u.scalar == PIR_SCALAR_PTR)) {
            pir_diag_error(v->diag, line, 0u, "'%s': noalias/readonly only valid on ptr parameter", owner);
        }
    }
}

static bool is_atomic_type(PirType t) {
    if (t.kind != PIR_TYPE_SCALAR) return false;
    switch (t.u.scalar) {
        case PIR_SCALAR_I8:
        case PIR_SCALAR_I16:
        case PIR_SCALAR_I32:
        case PIR_SCALAR_I64:
        case PIR_SCALAR_PTR:
            return true;
        case PIR_SCALAR_VOID: case PIR_SCALAR_I128:
        case PIR_SCALAR_F32:  case PIR_SCALAR_F64:
        case PIR_SCALAR_VALIST:
            return false;
    }
    return false;
}

static bool is_ptr_type(PirType t) {
    return t.kind == PIR_TYPE_SCALAR && t.u.scalar == PIR_SCALAR_PTR;
}

static bool is_i8_type(PirType t) {
    return t.kind == PIR_TYPE_SCALAR && t.u.scalar == PIR_SCALAR_I8;
}

static bool is_i32_or_i64(PirType t) {
    return t.kind == PIR_TYPE_SCALAR &&
           (t.u.scalar == PIR_SCALAR_I32 || t.u.scalar == PIR_SCALAR_I64);
}

static uint32_t popcount_u32(uint32_t x) {
    uint32_t c = 0u;
    while (x) { c += (x & 1u); x >>= 1u; }
    return c;
}

static void check_flags(Verifier *v, const PirInst *inst) {
    uint32_t f = inst->flags;
    if (f == PIR_FLAG_NONE) return;
    uint32_t allowed = PIR_FLAG_NONE;
    switch (inst->op) {
        case PIR_OP_ADD: case PIR_OP_SUB: case PIR_OP_MUL:
            allowed = PIR_FLAG_WRAP | PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U |
                      PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U;
            if ((f & ~allowed) != 0u) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "invalid semantic flag for integer add/sub/mul");
                return;
            }
            /* Allowed combinations: single flag, or [unsafe_s, unsafe_u]. */
            if (popcount_u32(f) > 1u && f != (PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "conflicting semantic flags on add/sub/mul");
            }
            return;
        case PIR_OP_DIV_S: case PIR_OP_DIV_U: case PIR_OP_REM_S: case PIR_OP_REM_U:
            allowed = PIR_FLAG_TRAP | PIR_FLAG_WRAP | PIR_FLAG_UNSAFE;
            break;
        case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR:
            allowed = PIR_FLAG_MASK | PIR_FLAG_TRAP | PIR_FLAG_UNSAFE;
            break;
        case PIR_OP_CLZ: case PIR_OP_CTZ:
            allowed = PIR_FLAG_NONZERO;
            break;
        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV:
            allowed = PIR_FLAG_FAST;
            break;
        case PIR_OP_FPTOSI: case PIR_OP_FPTOUI:
            allowed = PIR_FLAG_SAT | PIR_FLAG_TRAP | PIR_FLAG_UNSAFE;
            break;
        case PIR_OP_PTR_ADD:
            allowed = PIR_FLAG_WRAP | PIR_FLAG_INBOUNDS;
            break;
        case PIR_OP_LOAD: case PIR_OP_STORE:
            allowed = PIR_FLAG_VOLATILE;
            break;
        case PIR_OP_ATOMIC_CMPXCHG:
            allowed = PIR_FLAG_WEAK;
            break;
        case PIR_OP_CALL: case PIR_OP_CALL_INDIRECT:
            allowed = PIR_FLAG_NOUNWIND;
            break;
        case PIR_OP_CONST: case PIR_OP_ADDR:
        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
        case PIR_OP_MUL_WIDE_U: case PIR_OP_MUL_WIDE_S:
        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
        case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR:
        case PIR_OP_POPCNT: case PIR_OP_BSWAP:
        case PIR_OP_FNEG:
        case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_S: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_S: case PIR_OP_CMP_GE_U:
        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
        case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
        case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE:
        case PIR_OP_SELECT:
        case PIR_OP_ZEXT: case PIR_OP_SEXT: case PIR_OP_TRUNC:
        case PIR_OP_SITOFP: case PIR_OP_UITOFP:
        case PIR_OP_FPEXT: case PIR_OP_FPTRUNC: case PIR_OP_BITCAST:
        case PIR_OP_PTRTOINT: case PIR_OP_INTTOPTR:
        case PIR_OP_SPLAT: case PIR_OP_EXTRACT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
        case PIR_OP_ALLOCA: case PIR_OP_ALLOCA_PTR: case PIR_OP_ALLOCA_DYN:
        case PIR_OP_STACKSAVE: case PIR_OP_STACKRESTORE:
        case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR:
        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
        case PIR_OP_ATOMIC_LOAD: case PIR_OP_ATOMIC_STORE:
        case PIR_OP_ATOMIC_RMW: case PIR_OP_FENCE:
        case PIR_OP_VA_START: case PIR_OP_VA_ARG: case PIR_OP_VA_ARG_BUF:
        case PIR_OP_VA_COPY: case PIR_OP_VA_END:
        case PIR_OP_JMP: case PIR_OP_BR: case PIR_OP_SWITCH:
        case PIR_OP_INVOKE: case PIR_OP_INVOKE_INDIRECT: case PIR_OP_RESUME:
        case PIR_OP_RET: case PIR_OP_TRAP: case PIR_OP_UNREACHABLE:
            allowed = PIR_FLAG_NONE;
            break;
    }
    if ((f & ~allowed) != 0u) {
        pir_diag_error(v->diag, inst->src_line, 0u, "invalid semantic flag for opcode");
    } else if (popcount_u32(f) > 1u) {
        pir_diag_error(v->diag, inst->src_line, 0u, "conflicting semantic flags on instruction");
    }
}

static void check_branch_target(Verifier *v, const PirFunction *fn, const PirInst *inst,
                                const PirBranchTarget *bt, bool allow_ret, bool allow_exn,
                                PirType invoke_ret_ty) {
    if (bt->target >= fn->blocks.len) {
        pir_diag_error(v->diag, inst->src_line, 0u, "invalid branch target block");
        return;
    }
    if (bt->target == 0u) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "cannot branch to entry block '.%s'", fn->blocks.data[0].label);
        return;
    }
    const PirBlock *dst = &fn->blocks.data[bt->target];
    if (bt->arg_count != dst->params.len) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "block argument count mismatch for '.%s' (expected %u, got %u)",
                       dst->label, dst->params.len, bt->arg_count);
        return;
    }
    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        const PirOperand *arg = &bt->args[k];
        PirType expected = fn->values.data[dst->params.data[k]].type;
        if (arg->kind == PIR_OPER_INVOKE_RET) {
            if (!allow_ret || (invoke_ret_ty.kind == PIR_TYPE_SCALAR &&
                               invoke_ret_ty.u.scalar == PIR_SCALAR_VOID)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "'ret' placeholder only valid in non-void invoke 'then' edge");
                continue;
            }
        } else if (arg->kind == PIR_OPER_INVOKE_EXN) {
            if (!allow_exn) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "'exn' placeholder only valid in invoke 'unwind' edge");
                continue;
            }
        }
        if (!pir_type_eq(arg->type, expected)) {
            char b1[64], b2[64];
            pir_diag_error(v->diag, inst->src_line, 0u,
                           "block argument %u type mismatch for '.%s' (expected %s, got %s)",
                           k, dst->label,
                           type_str(v->mod, expected, b1, sizeof(b1)),
                           type_str(v->mod, arg->type, b2, sizeof(b2)));
        }
    }
}

static void check_call_args(Verifier *v, const PirInst *inst, const char *callee_name,
                            const PirParamDecl *params, uint32_t param_count,
                            bool is_variadic, uint32_t first_arg_idx) {
    uint32_t provided = inst->operand_count - first_arg_idx;
    if ((!is_variadic && provided != param_count) || (is_variadic && provided < param_count)) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "call to '%s': expected %s%u arguments, got %u",
                       callee_name, is_variadic ? "at least " : "", param_count, provided);
        return;
    }
    for (uint32_t i = 0u; i < provided; ++i) {
        const PirOperand *arg = &inst->operands[first_arg_idx + i];
        if (i < param_count) {
            if (!pir_type_eq(arg->type, params[i].type)) {
                char b1[64], b2[64];
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "call to '%s': argument %u type mismatch (expected %s, got %s)",
                               callee_name, i,
                               type_str(v->mod, params[i].type, b1, sizeof(b1)),
                               type_str(v->mod, arg->type, b2, sizeof(b2)));
            }
        } else {
            if (arg->call_abi == PIR_PARAM_BYVAL) {
                if (v->mode == PIR_VERIFY_LIR) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids byval at call site");
                }
                if (!is_ptr_type(arg->type)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "byval call argument must be ptr");
                }
            } else {
                check_ssa_type(v, arg->type, inst->src_line, "variadic call argument");
            }
        }
    }
}

static void check_dom_operand(Verifier *v, const PirFunction *fn, const PirDom *dom,
                              PirBlockId use_blk, uint32_t use_pos, const uint32_t *inst_pos,
                              const PirOperand *op, uint32_t line) {
    if (op->kind != PIR_OPER_VAL || op->u.val >= fn->values.len) return;
    const PirValueDef *vd = &fn->values.data[op->u.val];
    if (vd->def_block == PIR_INVALID_ID) {
        return; /* Function parameter dominates entire function. */
    }
    if (vd->def_block == use_blk) {
        if (vd->def_inst == PIR_INVALID_ID) return; /* Block param of same block. */
        if (inst_pos[vd->def_inst] >= use_pos) {
            pir_diag_error(v->diag, line, 0u,
                           "use of '%%%s' in '.%s' precedes its definition",
                           vd->name, fn->blocks.data[use_blk].label);
        }
        return;
    }
    if (!pir_dom_dominates(dom, vd->def_block, use_blk)) {
        pir_diag_error(v->diag, line, 0u,
                       "use of '%%%s' in '.%s' is not dominated by definition in '.%s'",
                       vd->name, fn->blocks.data[use_blk].label,
                       fn->blocks.data[vd->def_block].label);
    }
}

static void check_result_count(Verifier *v, const PirInst *inst, uint32_t expected) {
    uint32_t got = (inst->results[0] != PIR_INVALID_ID ? 1u : 0u) +
                   (inst->results[1] != PIR_INVALID_ID ? 1u : 0u);
    if (got != expected) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "instruction expects %u result(s), got %u", expected, got);
    }
}

static void check_op_count(Verifier *v, const PirInst *inst, uint32_t expected) {
    if (inst->operand_count != expected) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "instruction expects %u operand(s), got %u", expected, inst->operand_count);
    }
}

static bool fail_order_valid(PirMemOrder succ, PirMemOrder fail) {
    if (fail == PIR_ORDER_RELEASE || fail == PIR_ORDER_ACQ_REL || fail == PIR_ORDER_NONE) {
        return false;
    }
    if (fail == PIR_ORDER_RELAXED) return true;
    if (fail == PIR_ORDER_ACQUIRE) {
        return succ == PIR_ORDER_ACQUIRE || succ == PIR_ORDER_ACQ_REL || succ == PIR_ORDER_SEQ_CST;
    }
    if (fail == PIR_ORDER_SEQ_CST) {
        return succ == PIR_ORDER_SEQ_CST;
    }
    return false;
}

static void verify_inst(Verifier *v, const PirFunction *fn, const PirInst *inst) {
    check_flags(v, inst);
    uint8_t ptr_bytes = v->target ? v->target->ptr_bytes : 8u;

    /* Check LIR-legality of any defined result types. */
    if (inst->results[0] != PIR_INVALID_ID) {
        check_ssa_type(v, fn->values.data[inst->results[0]].type, inst->src_line, "result");
    }
    if (inst->results[1] != PIR_INVALID_ID) {
        check_ssa_type(v, fn->values.data[inst->results[1]].type, inst->src_line, "result");
    }

    switch (inst->op) {
        case PIR_OP_CONST:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            check_ssa_type(v, inst->ty, inst->src_line, "const");
            if (inst->operand_count == 1u && inst->operands[0].kind == PIR_OPER_SYM) {
                if (!is_ptr_type(inst->ty)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "symbol const requires ptr type");
                }
                if (!has_symbol(v->mod, inst->operands[0].u.sym.name)) {
                    pir_diag_error(v->diag, inst->src_line, 0u,
                                   "undefined symbol '@%s'", inst->operands[0].u.sym.name);
                }
            }
            break;

        case PIR_OP_ADDR:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u && (inst->operands[0].kind != PIR_OPER_SYM ||
                !has_symbol(v->mod, inst->operands[0].u.sym.name))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "addr requires defined @symbol");
            }
            break;

        case PIR_OP_ADD: case PIR_OP_SUB: case PIR_OP_MUL:
        case PIR_OP_DIV_S: case PIR_OP_DIV_U: case PIR_OP_REM_S: case PIR_OP_REM_U:
        case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR:
        case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!pir_type_is_int(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "integer opcode requires integer type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "integer op");
            if (inst->operand_count == 2u &&
                (!pir_type_eq(inst->operands[0].type, inst->ty) ||
                 !pir_type_eq(inst->operands[1].type, inst->ty))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "operand type mismatch on integer op");
            }
            break;

        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
            if (v->mode == PIR_VERIFY_LIR && v->target && !v->target->caps.has_ovf_ops) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR target does not support *_ovf_* ops");
            }
            check_result_count(v, inst, 2u);
            check_op_count(v, inst, 2u);
            if (!(inst->ty.kind == PIR_TYPE_SCALAR && pir_scalar_is_int(inst->ty.u.scalar))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "*_ovf_* requires scalar integer type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "ovf op");
            if (inst->operand_count == 2u &&
                (!pir_type_eq(inst->operands[0].type, inst->ty) ||
                 !pir_type_eq(inst->operands[1].type, inst->ty))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "operand type mismatch on *_ovf_*");
            }
            break;

        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
            if (v->mode == PIR_VERIFY_LIR && v->target &&
                (!v->target->caps.has_ovf_ops || !v->target->caps.has_add_carry)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR target does not support add_carry/sub_borrow");
            }
            check_result_count(v, inst, 2u);
            check_op_count(v, inst, 3u);
            if (!(inst->ty.kind == PIR_TYPE_SCALAR && pir_scalar_is_int(inst->ty.u.scalar))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "add_carry/sub_borrow requires scalar integer");
            }
            check_lir_type(v, inst->ty, inst->src_line, "carry/borrow op");
            if (inst->operand_count == 3u &&
                (!pir_type_eq(inst->operands[0].type, inst->ty) ||
                 !pir_type_eq(inst->operands[1].type, inst->ty) ||
                 !is_i8_type(inst->operands[2].type))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "operand type mismatch on add_carry/sub_borrow");
            }
            break;

        case PIR_OP_MUL_WIDE_U: case PIR_OP_MUL_WIDE_S:
            if (v->mode == PIR_VERIFY_LIR && v->target && !v->target->caps.has_mul_wide) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR target does not support mul_wide_*");
            }
            check_result_count(v, inst, 2u);
            check_op_count(v, inst, 2u);
            if (!is_i32_or_i64(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "mul_wide_* requires i32 or i64");
            }
            if (inst->operand_count == 2u &&
                (!pir_type_eq(inst->operands[0].type, inst->ty) ||
                 !pir_type_eq(inst->operands[1].type, inst->ty))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "operand type mismatch on mul_wide_*");
            }
            break;

        case PIR_OP_CLZ: case PIR_OP_CTZ: case PIR_OP_POPCNT: case PIR_OP_BSWAP:
            if (v->mode == PIR_VERIFY_LIR && v->target && !v->target->caps.has_bitops) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR target does not support bitops");
            }
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!(inst->ty.kind == PIR_TYPE_SCALAR && pir_scalar_is_int(inst->ty.u.scalar))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "bitop requires scalar integer");
            }
            if (inst->op == PIR_OP_BSWAP && is_i8_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "bswap is not valid on i8");
            }
            check_lir_type(v, inst->ty, inst->src_line, "bitop");
            if (inst->operand_count == 1u && !pir_type_eq(inst->operands[0].type, inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "operand type mismatch on bitop");
            }
            break;

        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!pir_type_is_float(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "float op requires float type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "float op");
            if (inst->operand_count == 2u &&
                (!pir_type_eq(inst->operands[0].type, inst->ty) ||
                 !pir_type_eq(inst->operands[1].type, inst->ty))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "operand type mismatch on float op");
            }
            break;

        case PIR_OP_FNEG:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!pir_type_is_float(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "fneg requires float type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "fneg");
            if (inst->operand_count == 1u && !pir_type_eq(inst->operands[0].type, inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "operand type mismatch on fneg");
            }
            break;

        case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_U:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!pir_type_is_int(inst->ty) && !is_ptr_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "cmp requires integer or ptr type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "cmp");
            if (inst->operand_count == 2u &&
                (!pir_type_eq(inst->operands[0].type, inst->ty) ||
                 !pir_type_eq(inst->operands[1].type, inst->ty))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "operand type mismatch on cmp");
            }
            break;

        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LE_S:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GE_S:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!pir_type_is_int(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "signed cmp requires integer type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "signed cmp");
            if (inst->operand_count == 2u &&
                (!pir_type_eq(inst->operands[0].type, inst->ty) ||
                 !pir_type_eq(inst->operands[1].type, inst->ty))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "operand type mismatch on signed cmp");
            }
            break;

        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
        case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
        case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!pir_type_is_float(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "fcmp requires float type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "fcmp");
            if (inst->operand_count == 2u &&
                (!pir_type_eq(inst->operands[0].type, inst->ty) ||
                 !pir_type_eq(inst->operands[1].type, inst->ty))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "operand type mismatch on fcmp");
            }
            break;

        case PIR_OP_SELECT:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 3u);
            check_ssa_type(v, inst->ty, inst->src_line, "select");
            if (inst->operand_count == 3u) {
                PirType cond_ty = (inst->ty.kind == PIR_TYPE_VECTOR)
                                  ? pir_type_vec(pir_vec_mask_type(inst->ty.u.vec))
                                  : pir_type_scalar(PIR_SCALAR_I8);
                if (!pir_type_eq(inst->operands[0].type, cond_ty)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "select condition type mismatch");
                }
                if (!pir_type_eq(inst->operands[1].type, inst->ty) ||
                    !pir_type_eq(inst->operands[2].type, inst->ty)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "select value type mismatch");
                }
            }
            break;

        case PIR_OP_ZEXT: case PIR_OP_SEXT: case PIR_OP_TRUNC:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!pir_type_is_int(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "int cast requires integer dst type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "int cast");
            if (inst->operand_count == 1u) {
                PirType src = inst->operands[0].type;
                if (!pir_type_is_int(src) || src.kind != inst->ty.kind) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "int cast requires matching int src kind");
                } else {
                    uint32_t sb = (src.kind == PIR_TYPE_VECTOR)
                                  ? pir_scalar_bits(pir_vec_elem_scalar(src.u.vec), ptr_bytes)
                                  : pir_scalar_bits(src.u.scalar, ptr_bytes);
                    uint32_t db = (inst->ty.kind == PIR_TYPE_VECTOR)
                                  ? pir_scalar_bits(pir_vec_elem_scalar(inst->ty.u.vec), ptr_bytes)
                                  : pir_scalar_bits(inst->ty.u.scalar, ptr_bytes);
                    if (src.kind == PIR_TYPE_VECTOR &&
                        pir_vec_lanes(src.u.vec) != pir_vec_lanes(inst->ty.u.vec)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "vector cast lane count mismatch");
                    }
                    if (inst->op == PIR_OP_TRUNC ? (db >= sb) : (db <= sb)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "invalid integer cast bit-width direction");
                    }
                }
            }
            break;

        case PIR_OP_FPEXT: case PIR_OP_FPTRUNC:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!pir_type_is_float(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "fpext/fptrunc requires float dst");
            }
            check_lir_type(v, inst->ty, inst->src_line, "fp cast");
            if (inst->operand_count == 1u) {
                PirType src = inst->operands[0].type;
                if (!pir_type_is_float(src) || src.kind != inst->ty.kind) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "fpext/fptrunc requires float src");
                } else {
                    PirScalarType ss = (src.kind == PIR_TYPE_VECTOR) ? pir_vec_elem_scalar(src.u.vec) : src.u.scalar;
                    PirScalarType ds = (inst->ty.kind == PIR_TYPE_VECTOR) ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar;
                    if (src.kind == PIR_TYPE_VECTOR &&
                        pir_vec_lanes(src.u.vec) != pir_vec_lanes(inst->ty.u.vec)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "vector fp cast lane mismatch");
                    }
                    if (inst->op == PIR_OP_FPEXT && !(ss == PIR_SCALAR_F32 && ds == PIR_SCALAR_F64)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "fpext requires f32 -> f64");
                    }
                    if (inst->op == PIR_OP_FPTRUNC && !(ss == PIR_SCALAR_F64 && ds == PIR_SCALAR_F32)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "fptrunc requires f64 -> f32");
                    }
                }
            }
            break;

        case PIR_OP_FPTOSI: case PIR_OP_FPTOUI:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!pir_type_is_int(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "fptosi/fptoui requires integer dst");
            }
            check_lir_type(v, inst->ty, inst->src_line, "fpto*");
            if (inst->operand_count == 1u) {
                PirType src = inst->operands[0].type;
                if (!pir_type_is_float(src) || src.kind != inst->ty.kind ||
                    (src.kind == PIR_TYPE_VECTOR &&
                     pir_vec_lanes(src.u.vec) != pir_vec_lanes(inst->ty.u.vec))) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "fptosi/fptoui requires matching float src");
                }
            }
            break;

        case PIR_OP_SITOFP: case PIR_OP_UITOFP:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!pir_type_is_float(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "sitofp/uitofp requires float dst");
            }
            check_lir_type(v, inst->ty, inst->src_line, "*tofp");
            if (inst->operand_count == 1u) {
                PirType src = inst->operands[0].type;
                if (!pir_type_is_int(src) || src.kind != inst->ty.kind ||
                    (src.kind == PIR_TYPE_VECTOR &&
                     pir_vec_lanes(src.u.vec) != pir_vec_lanes(inst->ty.u.vec))) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "sitofp/uitofp requires matching int src");
                }
            }
            break;

        case PIR_OP_PTRTOINT:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!is_i32_or_i64(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "ptrtoint dst must be i32 or i64");
            }
            if (inst->operand_count == 1u && !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "ptrtoint src must be ptr");
            }
            break;

        case PIR_OP_INTTOPTR:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!is_ptr_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "inttoptr dst must be ptr");
            }
            if (inst->operand_count == 1u && !is_i32_or_i64(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "inttoptr src must be i32 or i64");
            }
            break;

        case PIR_OP_BITCAST:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            check_ssa_type(v, inst->ty, inst->src_line, "bitcast");
            if (inst->operand_count == 1u) {
                uint32_t sb = pir_type_bits(inst->operands[0].type, ptr_bytes);
                uint32_t db = pir_type_bits(inst->ty, ptr_bytes);
                if (sb == 0u || sb != db) {
                    pir_diag_error(v->diag, inst->src_line, 0u,
                                   "bitcast bit-width mismatch (%u vs %u)", sb, db);
                }
            }
            break;

        case PIR_OP_SPLAT:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (inst->ty.kind != PIR_TYPE_VECTOR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "splat requires vector type");
            } else {
                check_lir_type(v, inst->ty, inst->src_line, "splat");
                PirType elem = pir_type_scalar(pir_vec_elem_scalar(inst->ty.u.vec));
                if (inst->operand_count == 1u && !pir_type_eq(inst->operands[0].type, elem)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "splat scalar does not match vector lane type");
                }
            }
            break;

        case PIR_OP_EXTRACT:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (inst->ty.kind != PIR_TYPE_VECTOR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "extract requires vector type");
            } else {
                check_lir_type(v, inst->ty, inst->src_line, "extract");
                if (inst->operand_count == 1u && !pir_type_eq(inst->operands[0].type, inst->ty)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "extract operand type mismatch");
                }
                if (inst->imm_u64 >= pir_vec_lanes(inst->ty.u.vec)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "extract lane index out of bounds");
                }
            }
            break;

        case PIR_OP_INSERT:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (inst->ty.kind != PIR_TYPE_VECTOR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "insert requires vector type");
            } else {
                check_lir_type(v, inst->ty, inst->src_line, "insert");
                PirType elem = pir_type_scalar(pir_vec_elem_scalar(inst->ty.u.vec));
                if (inst->operand_count == 2u &&
                    (!pir_type_eq(inst->operands[0].type, inst->ty) ||
                     !pir_type_eq(inst->operands[1].type, elem))) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "insert operand type mismatch");
                }
                if (inst->imm_u64 >= pir_vec_lanes(inst->ty.u.vec)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "insert lane index out of bounds");
                }
            }
            break;

        case PIR_OP_SHUFFLE:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (inst->ty.kind != PIR_TYPE_VECTOR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "shuffle requires vector type");
            } else {
                check_lir_type(v, inst->ty, inst->src_line, "shuffle");
                uint32_t lanes = pir_vec_lanes(inst->ty.u.vec);
                if (inst->operand_count == 2u &&
                    (!pir_type_eq(inst->operands[0].type, inst->ty) ||
                     !pir_type_eq(inst->operands[1].type, inst->ty))) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "shuffle operand type mismatch");
                }
                if (inst->shuffle_len != lanes) {
                    pir_diag_error(v->diag, inst->src_line, 0u,
                                   "shuffle mask length %u does not match %u lanes",
                                   inst->shuffle_len, lanes);
                }
                for (uint32_t k = 0u; k < inst->shuffle_len; ++k) {
                    if (inst->shuffle_mask[k] >= 2u * lanes) {
                        pir_diag_error(v->diag, inst->src_line, 0u,
                                       "shuffle mask index %u out of bounds (max %u)",
                                       (unsigned)inst->shuffle_mask[k], 2u * lanes - 1u);
                    }
                }
            }
            break;

        case PIR_OP_ALLOCA:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids typed 'alloca' (use alloca.ptr)");
            }
            check_result_count(v, inst, 1u);
            if (inst->aux_type.kind == PIR_TYPE_SCALAR && inst->aux_type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(v->diag, inst->src_line, 0u, "cannot alloca void");
            }
            if (inst->align != 0u && !pir_is_pow2_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a non-zero power of two", inst->align);
            }
            break;

        case PIR_OP_ALLOCA_PTR:
            check_result_count(v, inst, 1u);
            if (inst->imm_u64 == 0u) {
                pir_diag_error(v->diag, inst->src_line, 0u, "alloca.ptr byte size must be > 0");
            }
            if (!pir_is_pow2_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a non-zero power of two", inst->align);
            }
            break;

        case PIR_OP_ALLOCA_DYN:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u && !is_i32_or_i64(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "alloca_dyn size must be i32 or i64");
            }
            if (!pir_is_pow2_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a non-zero power of two", inst->align);
            }
            break;

        case PIR_OP_STACKSAVE:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 0u);
            break;

        case PIR_OP_STACKRESTORE:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u && !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "stackrestore operand must be ptr");
            }
            break;

        case PIR_OP_FIELD_PTR:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'field_ptr'");
                break;
            }
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u && !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "field_ptr base must be ptr");
            }
            if (inst->aux_type.kind != PIR_TYPE_NAMED_AGG ||
                inst->aux_type.u.agg_id >= v->mod->agg_types.len) {
                pir_diag_error(v->diag, inst->src_line, 0u, "field_ptr requires struct/union @Type");
            } else {
                const PirAggDef *agg = &v->mod->agg_types.data[inst->aux_type.u.agg_id];
                if (agg->kind == PIR_AGG_ARRAY) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "field_ptr cannot index array type");
                } else if (inst->imm_u64 >= agg->field_count) {
                    pir_diag_error(v->diag, inst->src_line, 0u,
                                   "field_ptr index %" PRIu64 " out of bounds for '@%s' (%u fields)",
                                   inst->imm_u64, agg->name ? agg->name : "anon", agg->field_count);
                }
            }
            break;

        case PIR_OP_ELEM_PTR:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'elem_ptr'");
                break;
            }
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (inst->aux_type.kind == PIR_TYPE_SCALAR && inst->aux_type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(v->diag, inst->src_line, 0u, "elem_ptr element type cannot be void");
            }
            if (inst->operand_count == 2u &&
                (!is_ptr_type(inst->operands[0].type) || !is_i32_or_i64(inst->operands[1].type))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "elem_ptr requires (ptr, i32|i64)");
            }
            break;

        case PIR_OP_PTR_ADD:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!is_i32_or_i64(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "ptr_add index type must be i32 or i64");
            }
            if (inst->operand_count == 2u &&
                (!is_ptr_type(inst->operands[0].type) || !pir_type_eq(inst->operands[1].type, inst->ty))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "ptr_add operand type mismatch");
            }
            break;

        case PIR_OP_LOAD:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            check_ssa_type(v, inst->ty, inst->src_line, "load");
            if (inst->operand_count == 1u && !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "load address must be ptr");
            }
            if (!pir_is_pow2_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a non-zero power of two", inst->align);
            }
            break;

        case PIR_OP_STORE:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 2u);
            check_ssa_type(v, inst->ty, inst->src_line, "store");
            if (inst->operand_count == 2u &&
                (!pir_type_eq(inst->operands[0].type, inst->ty) || !is_ptr_type(inst->operands[1].type))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "store operand type mismatch");
            }
            if (!pir_is_pow2_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a non-zero power of two", inst->align);
            }
            break;

        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only memcpy/memmove/memset");
                break;
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 3u);
            if (inst->operand_count == 3u) {
                if (!is_ptr_type(inst->operands[0].type) ||
                    (inst->op == PIR_OP_MEMSET ? !is_i8_type(inst->operands[1].type)
                                               : !is_ptr_type(inst->operands[1].type)) ||
                    !is_i32_or_i64(inst->operands[2].type)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "mem* operand type mismatch");
                }
            }
            if (!pir_is_pow2_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a non-zero power of two", inst->align);
            }
            break;

        case PIR_OP_ATOMIC_LOAD:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!is_atomic_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid atomic_load type");
            }
            if (inst->operand_count == 1u && !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "atomic_load address must be ptr");
            }
            if (inst->order != PIR_ORDER_RELAXED && inst->order != PIR_ORDER_ACQUIRE &&
                inst->order != PIR_ORDER_SEQ_CST) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid memory ordering for atomic_load");
            }
            break;

        case PIR_OP_ATOMIC_STORE:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 2u);
            if (!is_atomic_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid atomic_store type");
            }
            if (inst->operand_count == 2u &&
                (!pir_type_eq(inst->operands[0].type, inst->ty) || !is_ptr_type(inst->operands[1].type))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "atomic_store operand type mismatch");
            }
            if (inst->order != PIR_ORDER_RELAXED && inst->order != PIR_ORDER_RELEASE &&
                inst->order != PIR_ORDER_SEQ_CST) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid memory ordering for atomic_store");
            }
            break;

        case PIR_OP_ATOMIC_RMW:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!is_atomic_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid atomic_rmw type");
            }
            if (inst->operand_count == 2u &&
                (!is_ptr_type(inst->operands[0].type) || !pir_type_eq(inst->operands[1].type, inst->ty))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "atomic_rmw operand type mismatch");
            }
            if (inst->order == PIR_ORDER_NONE) {
                pir_diag_error(v->diag, inst->src_line, 0u, "missing memory ordering for atomic_rmw");
            }
            break;

        case PIR_OP_ATOMIC_CMPXCHG:
            check_result_count(v, inst, 2u);
            check_op_count(v, inst, 3u);
            if (!is_atomic_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid atomic_cmpxchg type");
            }
            if (inst->operand_count == 3u &&
                (!is_ptr_type(inst->operands[0].type) ||
                 !pir_type_eq(inst->operands[1].type, inst->ty) ||
                 !pir_type_eq(inst->operands[2].type, inst->ty))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "atomic_cmpxchg operand type mismatch");
            }
            if (!fail_order_valid(inst->order, inst->fail_order)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "invalid atomic_cmpxchg failure ordering (cannot be release/acq_rel or stronger than success)");
            }
            break;

        case PIR_OP_FENCE:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 0u);
            if (inst->order != PIR_ORDER_ACQUIRE && inst->order != PIR_ORDER_RELEASE &&
                inst->order != PIR_ORDER_ACQ_REL && inst->order != PIR_ORDER_SEQ_CST) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid memory ordering for fence");
            }
            break;

        case PIR_OP_VA_START:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (!fn->is_variadic || fn->cc != PIR_CC_C) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "va_start is only valid inside a variadic cc \"c\" function");
            }
            if (inst->operand_count == 1u && !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "va_start operand must be ptr");
            }
            break;

        case PIR_OP_VA_ARG:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'va_arg'");
                break;
            }
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            check_ssa_type(v, inst->ty, inst->src_line, "va_arg");
            if (inst->operand_count == 1u && !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "va_arg operand must be ptr");
            }
            break;

        case PIR_OP_VA_ARG_BUF:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'va_arg_buf'");
                break;
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 2u);
            if (inst->aux_type.kind != PIR_TYPE_NAMED_AGG) {
                pir_diag_error(v->diag, inst->src_line, 0u, "va_arg_buf requires aggregate type");
            }
            if (inst->operand_count == 2u &&
                (!is_ptr_type(inst->operands[0].type) || !is_ptr_type(inst->operands[1].type))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "va_arg_buf requires (ptr, ptr)");
            }
            break;

        case PIR_OP_VA_COPY:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'va_copy'");
                break;
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 2u);
            if (inst->operand_count == 2u &&
                (!is_ptr_type(inst->operands[0].type) || !is_ptr_type(inst->operands[1].type))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "va_copy requires (ptr, ptr)");
            }
            break;

        case PIR_OP_VA_END:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'va_end'");
                break;
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u && !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "va_end operand must be ptr");
            }
            break;

        case PIR_OP_CALL: {
            if (inst->results[1] != PIR_INVALID_ID) {
                pir_diag_error(v->diag, inst->src_line, 0u, "call produces at most 1 result");
            }
            if (inst->operand_count < 1u || !inst->operands[0].u.sym.is_func ||
                inst->operands[0].u.sym.id >= v->mod->funcs.len) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "call target '@%s' is not a declared function",
                               inst->operand_count >= 1u ? inst->operands[0].u.sym.name : "?");
                break;
            }
            const PirFunction *callee = &v->mod->funcs.data[inst->operands[0].u.sym.id];
            if (inst->results[0] != PIR_INVALID_ID &&
                callee->ret_type.kind == PIR_TYPE_SCALAR &&
                callee->ret_type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "cannot assign result of void call '@%s'", callee->name);
            }
            check_call_args(v, inst, callee->name, callee->params, callee->param_count,
                            callee->is_variadic, 1u);
            break;
        }

        case PIR_OP_CALL_INDIRECT: {
            if (inst->results[1] != PIR_INVALID_ID) {
                pir_diag_error(v->diag, inst->src_line, 0u, "call_indirect produces at most 1 result");
            }
            if (inst->sig_id >= v->mod->sigs.len) {
                pir_diag_error(v->diag, inst->src_line, 0u, "call_indirect references unknown signature");
                break;
            }
            if (inst->operand_count < 1u || !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "call_indirect function pointer must be ptr");
            }
            const PirSigDef *sig = &v->mod->sigs.data[inst->sig_id];
            if (inst->results[0] != PIR_INVALID_ID &&
                sig->ret_type.kind == PIR_TYPE_SCALAR &&
                sig->ret_type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "cannot assign result of void call_indirect '@%s'", sig->name);
            }
            check_call_args(v, inst, sig->name, sig->params, sig->param_count, sig->is_variadic, 1u);
            break;
        }

        case PIR_OP_JMP:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 0u);
            check_branch_target(v, fn, inst, &inst->targets[0], false, false,
                                pir_type_scalar(PIR_SCALAR_VOID));
            break;

        case PIR_OP_BR:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u && !is_i8_type(inst->operands[0].type)) {
                char b[64];
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "br condition must have type i8, got %s",
                               type_str(v->mod, inst->operands[0].type, b, sizeof(b)));
            }
            check_branch_target(v, fn, inst, &inst->targets[0], false, false,
                                pir_type_scalar(PIR_SCALAR_VOID));
            check_branch_target(v, fn, inst, &inst->targets[1], false, false,
                                pir_type_scalar(PIR_SCALAR_VOID));
            break;

        case PIR_OP_SWITCH:
            if (v->mode == PIR_VERIFY_LIR && v->target && !v->target->caps.has_switch) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR target does not support switch");
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (!(inst->ty.kind == PIR_TYPE_SCALAR && pir_scalar_is_int(inst->ty.u.scalar))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "switch requires scalar integer type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "switch");
            if (inst->operand_count == 1u && !pir_type_eq(inst->operands[0].type, inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "switch scrutinee type mismatch");
            }
            check_branch_target(v, fn, inst, &inst->targets[0], false, false,
                                pir_type_scalar(PIR_SCALAR_VOID));
            for (uint32_t c = 0u; c < inst->switch_case_count; ++c) {
                check_branch_target(v, fn, inst, &inst->switch_cases[c].target, false, false,
                                    pir_type_scalar(PIR_SCALAR_VOID));
                for (uint32_t d = 0u; d < c; ++d) {
                    if (inst->switch_cases[c].case_lo == inst->switch_cases[d].case_lo &&
                        inst->switch_cases[c].case_hi == inst->switch_cases[d].case_hi &&
                        inst->switch_cases[c].is_negative == inst->switch_cases[d].is_negative) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "duplicate switch case constant");
                        break;
                    }
                }
            }
            break;

        case PIR_OP_INVOKE: {
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'invoke'");
                break;
            }
            check_result_count(v, inst, 0u);
            if (inst->operand_count < 1u || !inst->operands[0].u.sym.is_func ||
                inst->operands[0].u.sym.id >= v->mod->funcs.len) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invoke target is not a declared function");
                break;
            }
            const PirFunction *callee = &v->mod->funcs.data[inst->operands[0].u.sym.id];
            check_call_args(v, inst, callee->name, callee->params, callee->param_count,
                            callee->is_variadic, 1u);
            check_branch_target(v, fn, inst, &inst->targets[0], true, false, callee->ret_type);
            check_branch_target(v, fn, inst, &inst->targets[1], false, true, callee->ret_type);
            break;
        }

        case PIR_OP_INVOKE_INDIRECT: {
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'invoke_indirect'");
                break;
            }
            check_result_count(v, inst, 0u);
            if (inst->sig_id >= v->mod->sigs.len) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invoke_indirect references unknown signature");
                break;
            }
            if (inst->operand_count < 1u || !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invoke_indirect callee must be ptr");
            }
            const PirSigDef *sig = &v->mod->sigs.data[inst->sig_id];
            check_call_args(v, inst, sig->name, sig->params, sig->param_count, sig->is_variadic, 1u);
            check_branch_target(v, fn, inst, &inst->targets[0], true, false, sig->ret_type);
            check_branch_target(v, fn, inst, &inst->targets[1], false, true, sig->ret_type);
            break;
        }

        case PIR_OP_RESUME:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'resume'");
                break;
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u && !is_ptr_type(inst->operands[0].type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "resume operand must be ptr");
            }
            break;

        case PIR_OP_RET:
            check_result_count(v, inst, 0u);
            if (fn->ret_type.kind == PIR_TYPE_SCALAR && fn->ret_type.u.scalar == PIR_SCALAR_VOID) {
                check_op_count(v, inst, 0u);
            } else {
                check_op_count(v, inst, 1u);
                if (inst->operand_count == 1u && !pir_type_eq(inst->operands[0].type, fn->ret_type)) {
                    char b1[64], b2[64];
                    pir_diag_error(v->diag, inst->src_line, 0u,
                                   "ret value type mismatch (expected %s, got %s)",
                                   type_str(v->mod, fn->ret_type, b1, sizeof(b1)),
                                   type_str(v->mod, inst->operands[0].type, b2, sizeof(b2)));
                }
            }
            break;

        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 0u);
            break;
    }
}

static bool has_file_id(const PirModule *mod, PirFileId fid) {
    for (uint32_t i = 0u; i < mod->files.len; ++i) {
        if (mod->files.data[i].id == fid) return true;
    }
    return false;
}

static void verify_function(Verifier *v, const PirFunction *fn) {
    verify_params_and_ret(v, fn->name, fn->src_line, fn->cc, fn->params,
                          fn->param_count, fn->is_variadic, fn->ret_type);

    if (fn->linkage == PIR_LINK_EXTERN) {
        if (fn->is_defined) {
            pir_diag_error(v->diag, fn->src_line, 0u, "extern function '@%s' cannot have a body", fn->name);
        }
        return;
    }
    if (!fn->is_defined || fn->blocks.len == 0u) {
        pir_diag_error(v->diag, fn->src_line, 0u, "function '@%s' must define at least one block", fn->name);
        return;
    }

    /* Validate all loc directives reference declared file IDs. */
    for (uint32_t i = 1u; i < fn->locs.len; ++i) {
        if (!has_file_id(v->mod, fn->locs.data[i].file_id)) {
            pir_diag_error(v->diag, fn->src_line, 0u,
                           "loc references undeclared file id %u", fn->locs.data[i].file_id);
        }
    }

    /* Entry block must have 0 block parameters (PIR.md §7.1 #3). */
    if (fn->blocks.data[0].params.len != 0u) {
        pir_diag_error(v->diag, fn->blocks.data[0].src_line, 0u,
                       "entry block '.%s' must not declare block parameters",
                       fn->blocks.data[0].label);
    }

    /* Check block params, terminators, and record instruction positions. */
    uint32_t *inst_pos = (uint32_t *)malloc((fn->insts.len + 1u) * sizeof(uint32_t));
    if (!inst_pos) abort();
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t k = 0u; k < blk->params.len; ++k) {
            check_ssa_type(v, fn->values.data[blk->params.data[k]].type, blk->src_line, "block param");
        }
        if (blk->insts.len == 0u) {
            pir_diag_error(v->diag, blk->src_line, 0u,
                           "block '.%s' does not end with a terminator", blk->label);
            continue;
        }
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            PirInstId iid = blk->insts.data[j];
            inst_pos[iid] = j;
            const PirInst *inst = &fn->insts.data[iid];
            bool is_term = pir_op_is_terminator(inst->op);
            if (j + 1u < blk->insts.len && is_term) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "terminator appears in middle of block '.%s'", blk->label);
            } else if (j + 1u == blk->insts.len && !is_term) {
                pir_diag_error(v->diag, blk->src_line, 0u,
                               "block '.%s' does not end with a terminator", blk->label);
            }
            verify_inst(v, fn, inst);
        }
    }

    /* Build CFG & Dominators and verify strict SSA dominance. */
    PirCfg cfg;
    PirDom dom;
    pir_cfg_build(&cfg, fn);
    pir_dom_build(&dom, &cfg);

    if (cfg.preds[0].len != 0u) {
        pir_diag_error(v->diag, fn->blocks.data[0].src_line, 0u,
                       "entry block '.%s' cannot have predecessors", fn->blocks.data[0].label);
    }

    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        if (!cfg.reachable[b]) continue;
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *inst = &fn->insts.data[blk->insts.data[j]];
            for (uint32_t k = 0u; k < inst->operand_count; ++k) {
                check_dom_operand(v, fn, &dom, b, j, inst_pos, &inst->operands[k], inst->src_line);
            }
            if (inst->op == PIR_OP_JMP || inst->op == PIR_OP_BR ||
                inst->op == PIR_OP_INVOKE || inst->op == PIR_OP_INVOKE_INDIRECT ||
                inst->op == PIR_OP_SWITCH) {
                uint32_t nt = (inst->op == PIR_OP_BR || inst->op == PIR_OP_INVOKE ||
                               inst->op == PIR_OP_INVOKE_INDIRECT) ? 2u : 1u;
                for (uint32_t t = 0u; t < nt; ++t) {
                    for (uint32_t k = 0u; k < inst->targets[t].arg_count; ++k) {
                        check_dom_operand(v, fn, &dom, b, j, inst_pos,
                                          &inst->targets[t].args[k], inst->src_line);
                    }
                }
                if (inst->op == PIR_OP_SWITCH) {
                    for (uint32_t c = 0u; c < inst->switch_case_count; ++c) {
                        const PirBranchTarget *bt = &inst->switch_cases[c].target;
                        for (uint32_t k = 0u; k < bt->arg_count; ++k) {
                            check_dom_operand(v, fn, &dom, b, j, inst_pos, &bt->args[k], inst->src_line);
                        }
                    }
                }
            }
        }
    }

    pir_dom_destroy(&dom);
    pir_cfg_destroy(&cfg);
    free(inst_pos);
}

static void check_unique_symbol(Verifier *v, const char *name, uint32_t line,
                                const char **seen, uint32_t *seen_len) {
    for (uint32_t i = 0u; i < *seen_len; ++i) {
        if (strcmp(seen[i], name) == 0) {
            pir_diag_error(v->diag, line, 0u, "duplicate module symbol '@%s'", name);
            return;
        }
    }
    seen[(*seen_len)++] = name;
}

bool pir_verify_module(const PirModule *mod, PirVerifyMode mode,
                       const PirTargetSpec *target, PirDiag *diag) {
    PirTargetSpec parsed_target;
    if (!target) {
        if (mod->target_triple && pir_target_from_triple(mod->target_triple, &parsed_target)) {
            target = &parsed_target;
        }
    }

    Verifier v = { mod, mode, target, diag };

    if (!mod->target_triple || !target) {
        pir_diag_error(diag, 1u, 0u, "invalid or unsupported target triple '%s'",
                       mod->target_triple ? mod->target_triple : "(null)");
    }

    /* Unique file IDs. */
    for (uint32_t i = 0u; i < mod->files.len; ++i) {
        for (uint32_t j = 0u; j < i; ++j) {
            if (mod->files.data[i].id == mod->files.data[j].id) {
                pir_diag_error(diag, 0u, 0u, "duplicate file id %u", mod->files.data[i].id);
                break;
            }
        }
    }

    /* Unique module-level @symbols across types, sigs, globals, funcs. */
    uint32_t max_syms = mod->agg_types.len + mod->sigs.len + mod->globals.len + mod->funcs.len + 1u;
    const char **seen = (const char **)malloc(max_syms * sizeof(const char *));
    if (!seen) abort();
    uint32_t seen_len = 0u;

    /* Aggregates. */
    uint8_t *agg_color = (uint8_t *)calloc(mod->agg_types.len + 1u, 1u);
    if (!agg_color) abort();
    for (uint32_t i = 0u; i < mod->agg_types.len; ++i) {
        const PirAggDef *a = &mod->agg_types.data[i];
        if (a->name) {
            check_unique_symbol(&v, a->name, a->src_line, seen, &seen_len);
        }
        if (mode == PIR_VERIFY_LIR) {
            pir_diag_error(diag, a->src_line, 0u,
                           "LIR forbids aggregate type declarations%s%s%s",
                           a->name ? " '@" : "", a->name ? a->name : "",
                           a->name ? "'" : "");
            continue;
        }
        if (a->kind == PIR_AGG_ARRAY) {
            if (a->array_len == 0u) {
                pir_diag_error(diag, a->src_line, 0u, "array type length must be > 0");
            }
            if (a->elem_type.kind == PIR_TYPE_SCALAR && a->elem_type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(diag, a->src_line, 0u, "array element type cannot be void");
            }
        } else {
            if (a->field_count == 0u) {
                pir_diag_error(diag, a->src_line, 0u, "struct/union '@%s' must have at least one field",
                               a->name ? a->name : "anon");
            }
            for (uint32_t f = 0u; f < a->field_count; ++f) {
                if (a->fields[f].kind == PIR_TYPE_SCALAR && a->fields[f].u.scalar == PIR_SCALAR_VOID) {
                    pir_diag_error(diag, a->src_line, 0u, "struct/union field cannot have type void");
                }
            }
        }
        check_agg_cycles(&v, i, agg_color);
    }
    free(agg_color);

    /* Signatures. */
    for (uint32_t i = 0u; i < mod->sigs.len; ++i) {
        const PirSigDef *s = &mod->sigs.data[i];
        check_unique_symbol(&v, s->name, s->src_line, seen, &seen_len);
        verify_params_and_ret(&v, s->name, s->src_line, s->cc, s->params,
                              s->param_count, s->is_variadic, s->ret_type);
    }

    /* Globals. */
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        check_unique_symbol(&v, g->name, g->src_line, seen, &seen_len);
        if (g->align != 0u && !pir_is_pow2_u32(g->align)) {
            pir_diag_error(diag, g->src_line, 0u,
                           "alignment %u is not a non-zero power of two", g->align);
        }
        if (g->has_type) {
            if (g->type.kind == PIR_TYPE_SCALAR && g->type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(diag, g->src_line, 0u, "global '@%s' cannot have type void", g->name);
            }
            check_lir_type(&v, g->type, g->src_line, "global");
        }
        for (uint32_t k = 0u; k < g->item_count; ++k) {
            if (g->items[k].kind == PIR_GITEM_RELOC && !has_symbol(mod, g->items[k].sym_name)) {
                pir_diag_error(diag, g->src_line, 0u,
                               "global '@%s' references undefined symbol '@%s'",
                               g->name, g->items[k].sym_name);
            }
        }
    }

    /* Functions. */
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        const PirFunction *fn = &mod->funcs.data[i];
        check_unique_symbol(&v, fn->name, fn->src_line, seen, &seen_len);
        verify_function(&v, fn);
    }

    free(seen);
    return !pir_diag_has_errors(diag);
}
