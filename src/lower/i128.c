/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lower/lower.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    PirValueId lo;
    PirValueId hi;
} I128Pair;

typedef struct {
    PirValueId from;
    PirValueId to;
} ValAlias;

static bool is_i128(PirType t) {
    return t.kind == PIR_TYPE_SCALAR && t.u.scalar == PIR_SCALAR_I128;
}

static void split_i128_imm(const PirOperand *op, uint64_t *out_lo, uint64_t *out_hi) {
    uint64_t lo = op->u.imm_int.lo;
    uint64_t hi = op->u.imm_int.hi;
    if (op->u.imm_int.is_negative) {
        uint64_t nlo = ~lo + 1u;
        uint64_t carry = (nlo == 0u) ? 1u : 0u;
        lo = nlo;
        hi = ~hi + carry;
    }
    *out_lo = lo;
    *out_hi = hi;
}

static void operand_to_pair(const PirOperand *op, const I128Pair *map,
                            PirOperand *out_lo, PirOperand *out_hi) {
    PirType t64 = pir_type_scalar(PIR_SCALAR_I64);
    if (op->kind == PIR_OPER_VAL) {
        *out_lo = pir_op_val(map[op->u.val].lo, t64);
        *out_hi = pir_op_val(map[op->u.val].hi, t64);
    } else {
        uint64_t lo = 0u, hi = 0u;
        split_i128_imm(op, &lo, &hi);
        *out_lo = pir_op_imm_i(lo, false, t64);
        *out_hi = pir_op_imm_i(hi, false, t64);
    }
}

static PirValueId emit_bin(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                           PirOpcode op, PirType ty, uint32_t flags,
                           PirOperand a, PirOperand b, uint32_t line, uint32_t loc,
                           PirValueId preset_res) {
    PirInst ins = pir_make_inst(op, ty, line, loc);
    ins.flags         = flags;
    ins.operand_count = 2u;
    ins.operands      = pir_alloc_ops(mod, 2u);
    ins.operands[0]   = a;
    ins.operands[1]   = b;
    PirType rty = ((op >= PIR_OP_CMP_EQ && op <= PIR_OP_CMP_GE_U) ||
                   (op >= PIR_OP_FCMP_OEQ && op <= PIR_OP_FCMP_UGE))
                  ? pir_type_scalar(PIR_SCALAR_I8) : ty;
    ins.results[0] = (preset_res != PIR_INVALID_ID)
                     ? preset_res : pir_fn_new_val(mod, fn, "w", rty, blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

static PirValueId emit_sel(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                           PirType ty, PirOperand cond, PirOperand tv, PirOperand fv,
                           uint32_t line, uint32_t loc, PirValueId preset_res) {
    PirInst ins = pir_make_inst(PIR_OP_SELECT, ty, line, loc);
    ins.operand_count = 3u;
    ins.operands      = pir_alloc_ops(mod, 3u);
    ins.operands[0]   = cond;
    ins.operands[1]   = tv;
    ins.operands[2]   = fv;
    ins.results[0]    = (preset_res != PIR_INVALID_ID)
                        ? preset_res : pir_fn_new_val(mod, fn, "ws", ty, blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

static PirValueId emit_un(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                          PirOpcode op, PirType ty, PirOperand a, uint32_t line, uint32_t loc,
                          PirValueId preset_res) {
    PirInst ins = pir_make_inst(op, ty, line, loc);
    ins.operand_count = 1u;
    ins.operands      = pir_alloc_ops(mod, 1u);
    ins.operands[0]   = a;
    ins.results[0]    = (preset_res != PIR_INVALID_ID)
                        ? preset_res : pir_fn_new_val(mod, fn, "wu", ty, blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

static void emit_trap_if(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                         PirValueId cond_i8, uint32_t ln, uint32_t lc) {
    PirType t64 = pir_type_scalar(PIR_SCALAR_I64);
    PirType t8  = pir_type_scalar(PIR_SCALAR_I8);
    PirValueId c64 = emit_un(mod, fn, blk, out, PIR_OP_ZEXT, t64, pir_op_val(cond_i8, t8),
                             ln, lc, PIR_INVALID_ID);
    emit_bin(mod, fn, blk, out, PIR_OP_ADD, t64, PIR_FLAG_TRAP_U,
             pir_op_val(c64, t64), pir_op_imm_i(UINT64_MAX, false, t64),
             ln, lc, PIR_INVALID_ID);
}

static void emit_neg128(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                        PirOperand lo, PirOperand hi, uint32_t ln, uint32_t lc,
                        PirValueId *out_lo, PirValueId *out_hi) {
    PirType t64 = pir_type_scalar(PIR_SCALAR_I64);
    PirType t8  = pir_type_scalar(PIR_SCALAR_I8);
    PirInst s0 = pir_make_inst(PIR_OP_SUB_BORROW, t64, ln, lc);
    s0.operand_count = 3u; s0.operands = pir_alloc_ops(mod, 3u);
    s0.operands[0] = pir_op_imm_i(0u, false, t64);
    s0.operands[1] = lo;
    s0.operands[2] = pir_op_imm_i(0u, false, t8);
    s0.results[0]  = pir_fn_new_val(mod, fn, "nlo", t64, blk, PIR_INVALID_ID, 0u);
    s0.results[1]  = pir_fn_new_val(mod, fn, "nbw", t8,  blk, PIR_INVALID_ID, 1u);
    pir_fn_append_inst(fn, out, s0);

    PirInst s1 = pir_make_inst(PIR_OP_SUB_BORROW, t64, ln, lc);
    s1.operand_count = 3u; s1.operands = pir_alloc_ops(mod, 3u);
    s1.operands[0] = pir_op_imm_i(0u, false, t64);
    s1.operands[1] = hi;
    s1.operands[2] = pir_op_val(s0.results[1], t8);
    s1.results[0]  = pir_fn_new_val(mod, fn, "nhi", t64, blk, PIR_INVALID_ID, 0u);
    s1.results[1]  = pir_fn_new_val(mod, fn, "nbw2", t8, blk, PIR_INVALID_ID, 1u);
    pir_fn_append_inst(fn, out, s1);

    *out_lo = s0.results[0];
    *out_hi = s1.results[0];
}

static PirValueId emit_mul_ovf_u128(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                                    PirOperand alo, PirOperand ahi, PirOperand blo, PirOperand bhi,
                                    uint32_t ln, uint32_t lc, PirValueId rlo, PirValueId rhi) {
    PirType t64 = pir_type_scalar(PIR_SCALAR_I64);
    PirType t8  = pir_type_scalar(PIR_SCALAR_I8);

    PirInst mw0 = pir_make_inst(PIR_OP_MUL_WIDE_U, t64, ln, lc);
    mw0.operand_count = 2u; mw0.operands = pir_alloc_ops(mod, 2u);
    mw0.operands[0] = alo; mw0.operands[1] = blo;
    mw0.results[0]  = (rlo != PIR_INVALID_ID) ? rlo : pir_fn_new_val(mod, fn, "mlo", t64, blk, PIR_INVALID_ID, 0u);
    mw0.results[1]  = pir_fn_new_val(mod, fn, "m0h", t64, blk, PIR_INVALID_ID, 1u);
    pir_fn_append_inst(fn, out, mw0);

    PirInst mw1 = pir_make_inst(PIR_OP_MUL_WIDE_U, t64, ln, lc);
    mw1.operand_count = 2u; mw1.operands = pir_alloc_ops(mod, 2u);
    mw1.operands[0] = alo; mw1.operands[1] = bhi;
    mw1.results[0]  = pir_fn_new_val(mod, fn, "m1l", t64, blk, PIR_INVALID_ID, 0u);
    mw1.results[1]  = pir_fn_new_val(mod, fn, "m1h", t64, blk, PIR_INVALID_ID, 1u);
    pir_fn_append_inst(fn, out, mw1);

    PirInst mw2 = pir_make_inst(PIR_OP_MUL_WIDE_U, t64, ln, lc);
    mw2.operand_count = 2u; mw2.operands = pir_alloc_ops(mod, 2u);
    mw2.operands[0] = ahi; mw2.operands[1] = blo;
    mw2.results[0]  = pir_fn_new_val(mod, fn, "m2l", t64, blk, PIR_INVALID_ID, 0u);
    mw2.results[1]  = pir_fn_new_val(mod, fn, "m2h", t64, blk, PIR_INVALID_ID, 1u);
    pir_fn_append_inst(fn, out, mw2);

    PirInst a1 = pir_make_inst(PIR_OP_ADD_CARRY, t64, ln, lc);
    a1.operand_count = 3u; a1.operands = pir_alloc_ops(mod, 3u);
    a1.operands[0] = pir_op_val(mw0.results[1], t64);
    a1.operands[1] = pir_op_val(mw1.results[0], t64);
    a1.operands[2] = pir_op_imm_i(0u, false, t8);
    a1.results[0]  = pir_fn_new_val(mod, fn, "mt0", t64, blk, PIR_INVALID_ID, 0u);
    a1.results[1]  = pir_fn_new_val(mod, fn, "mc1", t8,  blk, PIR_INVALID_ID, 1u);
    pir_fn_append_inst(fn, out, a1);

    PirInst a2 = pir_make_inst(PIR_OP_ADD_CARRY, t64, ln, lc);
    a2.operand_count = 3u; a2.operands = pir_alloc_ops(mod, 3u);
    a2.operands[0] = pir_op_val(a1.results[0], t64);
    a2.operands[1] = pir_op_val(mw2.results[0], t64);
    a2.operands[2] = pir_op_imm_i(0u, false, t8);
    a2.results[0]  = (rhi != PIR_INVALID_ID) ? rhi : pir_fn_new_val(mod, fn, "mhi", t64, blk, PIR_INVALID_ID, 0u);
    a2.results[1]  = pir_fn_new_val(mod, fn, "mc2", t8,  blk, PIR_INVALID_ID, 1u);
    pir_fn_append_inst(fn, out, a2);

    PirValueId ah_nz = emit_bin(mod, fn, blk, out, PIR_OP_CMP_NE, t64, 0u, ahi, pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
    PirValueId bh_nz = emit_bin(mod, fn, blk, out, PIR_OP_CMP_NE, t64, 0u, bhi, pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
    PirValueId both_hi = emit_bin(mod, fn, blk, out, PIR_OP_AND, t8, 0u, pir_op_val(ah_nz, t8), pir_op_val(bh_nz, t8), ln, lc, PIR_INVALID_ID);
    PirValueId hi_or = emit_bin(mod, fn, blk, out, PIR_OP_OR, t64, 0u, pir_op_val(mw1.results[1], t64), pir_op_val(mw2.results[1], t64), ln, lc, PIR_INVALID_ID);
    PirValueId hi_nz = emit_bin(mod, fn, blk, out, PIR_OP_CMP_NE, t64, 0u, pir_op_val(hi_or, t64), pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
    PirValueId cy_or = emit_bin(mod, fn, blk, out, PIR_OP_OR, t8, 0u, pir_op_val(a1.results[1], t8), pir_op_val(a2.results[1], t8), ln, lc, PIR_INVALID_ID);
    PirValueId o1 = emit_bin(mod, fn, blk, out, PIR_OP_OR, t8, 0u, pir_op_val(both_hi, t8), pir_op_val(hi_nz, t8), ln, lc, PIR_INVALID_ID);
    return emit_bin(mod, fn, blk, out, PIR_OP_OR, t8, 0u, pir_op_val(o1, t8), pir_op_val(cy_or, t8), ln, lc, PIR_INVALID_ID);
}

static void expand_branch_target_i128(PirModule *mod, PirBranchTarget *bt, const I128Pair *map) {
    if (bt->arg_count == 0u) return;
    bool has_any = false;
    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        if (is_i128(bt->args[k].type)) { has_any = true; break; }
    }
    if (!has_any) return;
    PIR_VEC(PirOperand) nargs = {0};
    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        if (is_i128(bt->args[k].type)) {
            PirOperand lo, hi;
            operand_to_pair(&bt->args[k], map, &lo, &hi);
            PIR_VEC_PUSH(&nargs, lo);
            PIR_VEC_PUSH(&nargs, hi);
        } else {
            PIR_VEC_PUSH(&nargs, bt->args[k]);
        }
    }
    bt->arg_count = nargs.len;
    bt->args      = pir_alloc_ops(mod, nargs.len);
    memcpy(bt->args, nargs.data, nargs.len * sizeof(PirOperand));
    PIR_VEC_FREE(&nargs);
}

static void legalize_i128_func(PirModule *mod, const PirTargetSpec *target, PirFuncId fi,
                               const bool *fn_ret_i128, const bool *sig_ret_i128,
                               uint32_t orig_funcs) {
    PirFunction *fn = &mod->funcs.data[fi];
    if (!fn->is_defined) return;

    bool ret_is_i128 = fn_ret_i128[fi];
    bool uses_i128 = ret_is_i128;
    if (!uses_i128) {
        for (uint32_t v = 0u; v < fn->values.len; ++v) {
            if (is_i128(fn->values.data[v].type)) { uses_i128 = true; break; }
        }
    }
    if (!uses_i128) {
        for (uint32_t i = 0u; i < fn->insts.len && !uses_i128; ++i) {
            const PirInst *ins = &fn->insts.data[i];
            if (is_i128(ins->ty)) { uses_i128 = true; break; }
            for (uint32_t k = 0u; k < ins->operand_count; ++k) {
                if (is_i128(ins->operands[k].type)) { uses_i128 = true; break; }
            }
            if (uses_i128) break;
            if (ins->op == PIR_OP_CALL && ins->operands[0].u.sym.is_func &&
                ins->operands[0].u.sym.id < orig_funcs &&
                fn_ret_i128[ins->operands[0].u.sym.id]) {
                uses_i128 = true;
                break;
            }
            if (ins->op == PIR_OP_CALL_INDIRECT && ins->sig_id < mod->sigs.len &&
                sig_ret_i128[ins->sig_id]) {
                uses_i128 = true;
                break;
            }
        }
    }
    if (!uses_i128) return;

    PirType t64 = pir_type_scalar(PIR_SCALAR_I64);
    PirType t8  = pir_type_scalar(PIR_SCALAR_I8);
    PirType tptr = pir_type_scalar(PIR_SCALAR_PTR);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);
    PirType idx_ty = pir_type_scalar(target->ptr_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);

    uint32_t orig_vals = fn->values.len;
    I128Pair *map = (I128Pair *)malloc((orig_vals + 1u) * sizeof(I128Pair));
    if (!map) abort();
    for (uint32_t v = 0u; v < orig_vals; ++v) {
        map[v].lo = PIR_INVALID_ID;
        map[v].hi = PIR_INVALID_ID;
        if (is_i128(fn->values.data[v].type)) {
            PirBlockId db = fn->values.data[v].def_block;
            uint32_t   pi = fn->values.data[v].param_index;
            /* Reuse original value slot as lo, allocate fresh value for hi. */
            fn->values.data[v].type = t64;
            map[v].lo = v;
            map[v].hi = pir_fn_new_val(mod, fn, "hi", t64, db, PIR_INVALID_ID, pi);
        }
    }

    /* Expand i128 block parameters into (lo, hi) pairs. */
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        PirValueIdVec nparams = {0};
        for (uint32_t k = 0u; k < fn->blocks.data[b].params.len; ++k) {
            PirValueId pv = fn->blocks.data[b].params.data[k];
            PIR_VEC_PUSH(&nparams, pv);
            if (pv < orig_vals && map[pv].hi != PIR_INVALID_ID) {
                PIR_VEC_PUSH(&nparams, map[pv].hi);
            }
        }
        for (uint32_t k = 0u; k < nparams.len; ++k) {
            fn->values.data[nparams.data[k]].param_index = k;
        }
        PIR_VEC_FREE(&fn->blocks.data[b].params);
        fn->blocks.data[b].params = nparams;
    }

    /* Expand i128 function parameters into (lo, hi) pairs and prepend hidden ptr if returning i128. */
    PirValueId ret_out_val = PIR_INVALID_ID;
    bool has_i128_param = false;
    for (uint32_t i = 0u; i < fn->param_count; ++i) {
        if (is_i128(fn->params[i].type)) { has_i128_param = true; break; }
    }
    if (has_i128_param || ret_is_i128) {
        PIR_VEC(PirParamDecl) nfp = {0};
        if (ret_is_i128) {
            ret_out_val = pir_fn_new_val(mod, fn, "i128_out", tptr, PIR_INVALID_ID, PIR_INVALID_ID, 0u);
            PirParamDecl op;
            memset(&op, 0, sizeof(op));
            op.name     = fn->values.data[ret_out_val].name;
            op.type     = tptr;
            op.abi_type = tvoid;
            op.val_id   = ret_out_val;
            PIR_VEC_PUSH(&nfp, op);

            fn->ret_type           = tvoid;
            fn->cabi_ret.kind      = PIR_CABI_RET_MULTI_REG;
            fn->cabi_ret.reg_count = 2u;
            fn->cabi_ret.regs[0]   = t64;
            fn->cabi_ret.regs[1]   = t64;
            fn->cabi_ret.agg_size  = 16u;
            fn->cabi_ret.agg_align = 8u;
        }
        for (uint32_t i = 0u; i < fn->param_count; ++i) {
            if (is_i128(fn->params[i].type)) {
                PirParamDecl p0 = fn->params[i];
                PirParamDecl p1 = fn->params[i];
                p0.type   = t64;
                p1.type   = t64;
                p1.abi_stack_align = 0u;
                p1.val_id = map[p0.val_id].hi;
                p1.name   = fn->values.data[p1.val_id].name;
                PIR_VEC_PUSH(&nfp, p0);
                PIR_VEC_PUSH(&nfp, p1);
            } else {
                PIR_VEC_PUSH(&nfp, fn->params[i]);
            }
        }
        for (uint32_t k = 0u; k < nfp.len; ++k) {
            if (nfp.data[k].val_id != PIR_INVALID_ID) {
                fn->values.data[nfp.data[k].val_id].param_index = k;
            }
        }
        fn->param_count = nfp.len;
        fn->params = PIR_ARENA_NEW_ARRAY(&mod->arena, PirParamDecl, nfp.len);
        memcpy(fn->params, nfp.data, nfp.len * sizeof(PirParamDecl));
        PIR_VEC_FREE(&nfp);
    }

    PIR_VEC(ValAlias) aliases = {0};

    uint32_t num_blocks = fn->blocks.len;
    for (uint32_t b = 0u; b < num_blocks; ++b) {
        PirInstIdVec new_insts = {0};
        uint32_t old_len = mod->funcs.data[fi].blocks.data[b].insts.len;

        for (uint32_t j = 0u; j < old_len; ++j) {
            PirInstId iid = mod->funcs.data[fi].blocks.data[b].insts.data[j];
            PirInst copy  = mod->funcs.data[fi].insts.data[iid];
            fn = &mod->funcs.data[fi];
            uint32_t ln = copy.src_line, lc = copy.loc_idx;

            /* Rewrite ret %i128 -> store (lo, hi) to hidden ret_out_val + ret void. */
            if (copy.op == PIR_OP_RET && ret_out_val != PIR_INVALID_ID && copy.operand_count == 1u) {
                PirOperand vlo, vhi;
                operand_to_pair(&copy.operands[0], map, &vlo, &vhi);
                PirInst s0 = pir_make_inst(PIR_OP_STORE, t64, ln, lc);
                s0.align = 8u; s0.operand_count = 2u; s0.operands = pir_alloc_ops(mod, 2u);
                s0.operands[0] = target->is_big_endian ? vhi : vlo;
                s0.operands[1] = pir_op_val(ret_out_val, tptr);
                pir_fn_append_inst(fn, &new_insts, s0);

                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = pir_op_val(ret_out_val, tptr);
                pa.operands[1] = pir_op_imm_i(8u, false, idx_ty);
                pa.results[0]  = pir_fn_new_val(mod, fn, "r128p", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);

                PirInst s1 = pir_make_inst(PIR_OP_STORE, t64, ln, lc);
                s1.align = 8u; s1.operand_count = 2u; s1.operands = pir_alloc_ops(mod, 2u);
                s1.operands[0] = target->is_big_endian ? vlo : vhi;
                s1.operands[1] = pir_op_val(pa.results[0], tptr);
                pir_fn_append_inst(fn, &new_insts, s1);

                PirInst rt = pir_make_inst(PIR_OP_RET, tvoid, ln, lc);
                pir_fn_append_inst(fn, &new_insts, rt);
                continue;
            }

            /* Split i128 arguments and returns at call / call_indirect sites. */
            if (copy.op == PIR_OP_CALL || copy.op == PIR_OP_CALL_INDIRECT) {
                bool call_ret_i128 = false;
                if (copy.op == PIR_OP_CALL && copy.operands[0].u.sym.is_func &&
                    copy.operands[0].u.sym.id < orig_funcs) {
                    call_ret_i128 = fn_ret_i128[copy.operands[0].u.sym.id];
                } else if (copy.op == PIR_OP_CALL_INDIRECT && copy.sig_id < mod->sigs.len) {
                    call_ret_i128 = sig_ret_i128[copy.sig_id];
                }
                bool has_i128_arg = false;
                for (uint32_t k = 1u; k < copy.operand_count; ++k) {
                    if (is_i128(copy.operands[k].type)) { has_i128_arg = true; break; }
                }
                if (call_ret_i128 || has_i128_arg) {
                    PirValueId rbuf = PIR_INVALID_ID;
                    if (call_ret_i128) {
                        PirInst al = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, ln, lc);
                        al.imm_u64 = 16u; al.align = 8u;
                        al.results[0] = pir_fn_new_val(mod, fn, "c128_out", tptr, b, PIR_INVALID_ID, 0u);
                        pir_fn_append_inst(fn, &new_insts, al);
                        rbuf = al.results[0];
                    }
                    PIR_VEC(PirOperand) nargs = {0};
                    PIR_VEC_PUSH(&nargs, copy.operands[0]);
                    if (call_ret_i128) {
                        PIR_VEC_PUSH(&nargs, pir_op_val(rbuf, tptr));
                    }
                    for (uint32_t k = 1u; k < copy.operand_count; ++k) {
                        if (is_i128(copy.operands[k].type)) {
                            PirOperand lo, hi;
                            operand_to_pair(&copy.operands[k], map, &lo, &hi);
                            lo.call_stack_align = copy.operands[k].call_stack_align;
                            hi.call_stack_align = 0u;
                            lo.call_force_stack = copy.operands[k].call_force_stack;
                            hi.call_force_stack = copy.operands[k].call_force_stack;
                            PIR_VEC_PUSH(&nargs, lo);
                            PIR_VEC_PUSH(&nargs, hi);
                        } else {
                            PIR_VEC_PUSH(&nargs, copy.operands[k]);
                        }
                    }
                    PirInst *slot = &fn->insts.data[iid];
                    slot->operand_count = nargs.len;
                    slot->operands      = pir_alloc_ops(mod, nargs.len);
                    memcpy(slot->operands, nargs.data, nargs.len * sizeof(PirOperand));
                    PIR_VEC_FREE(&nargs);
                    if (call_ret_i128) {
                        PirValueId orig_res = copy.results[0];
                        slot->ty         = tvoid;
                        slot->results[0] = PIR_INVALID_ID;
                        slot->results[1] = PIR_INVALID_ID;
                        PIR_VEC_PUSH(&new_insts, iid);
                        if (orig_res != PIR_INVALID_ID && orig_res < orig_vals) {
                            PirValueId rlo_c = map[orig_res].lo;
                            PirValueId rhi_c = map[orig_res].hi;
                            PirInst l0 = pir_make_inst(PIR_OP_LOAD, t64, ln, lc);
                            l0.align = 8u; l0.operand_count = 1u; l0.operands = pir_alloc_ops(mod, 1u);
                            l0.operands[0] = pir_op_val(rbuf, tptr);
                            l0.results[0]  = target->is_big_endian ? rhi_c : rlo_c;
                            pir_fn_append_inst(fn, &new_insts, l0);

                            PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
                            pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                            pa.operands[0] = pir_op_val(rbuf, tptr);
                            pa.operands[1] = pir_op_imm_i(8u, false, idx_ty);
                            pa.results[0]  = pir_fn_new_val(mod, fn, "c128_p1", tptr, b, PIR_INVALID_ID, 0u);
                            pir_fn_append_inst(fn, &new_insts, pa);

                            PirInst l1 = pir_make_inst(PIR_OP_LOAD, t64, ln, lc);
                            l1.align = 8u; l1.operand_count = 1u; l1.operands = pir_alloc_ops(mod, 1u);
                            l1.operands[0] = pir_op_val(pa.results[0], tptr);
                            l1.results[0]  = target->is_big_endian ? rlo_c : rhi_c;
                            pir_fn_append_inst(fn, &new_insts, l1);
                        }
                        continue;
                    }
                }
                PIR_VEC_PUSH(&new_insts, iid);
                continue;
            }

            /* Rewrite branch targets if any argument is i128 (and lower switch.i128 to cmp+br chain). */
            if (copy.op == PIR_OP_JMP || copy.op == PIR_OP_BR || copy.op == PIR_OP_SWITCH) {
                PirInst *slot = &fn->insts.data[iid];
                expand_branch_target_i128(mod, &slot->targets[0], map);
                if (copy.op == PIR_OP_BR) {
                    expand_branch_target_i128(mod, &slot->targets[1], map);
                } else if (copy.op == PIR_OP_SWITCH) {
                    for (uint32_t c = 0u; c < slot->switch_case_count; ++c) {
                        expand_branch_target_i128(mod, &slot->switch_cases[c].target, map);
                    }
                    if (is_i128(copy.ty)) {
                        PirInst sw_copy = *slot;
                        PirOperand vlo, vhi;
                        operand_to_pair(&sw_copy.operands[0], map, &vlo, &vhi);
                        if (sw_copy.switch_case_count == 0u) {
                            PirInst jm = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
                            jm.targets[0] = sw_copy.targets[0];
                            pir_fn_append_inst(fn, &new_insts, jm);
                        } else {
                            PirInstIdVec *cur_out = &new_insts;
                            PirBlockId cur_blk = b;
                            for (uint32_t c = 0u; c < sw_copy.switch_case_count; ++c) {
                                PirOperand clo, chi;
                                PirOperand cop = pir_op_imm_i(sw_copy.switch_cases[c].case_lo,
                                                              sw_copy.switch_cases[c].is_negative,
                                                              copy.ty);
                                cop.u.imm_int.hi = sw_copy.switch_cases[c].case_hi;
                                operand_to_pair(&cop, map, &clo, &chi);

                                PirValueId c0 = emit_bin(mod, fn, cur_blk, cur_out, PIR_OP_CMP_EQ, t64, 0u, vlo, clo, ln, lc, PIR_INVALID_ID);
                                PirValueId c1 = emit_bin(mod, fn, cur_blk, cur_out, PIR_OP_CMP_EQ, t64, 0u, vhi, chi, ln, lc, PIR_INVALID_ID);
                                PirValueId ceq = emit_bin(mod, fn, cur_blk, cur_out, PIR_OP_AND, t8, 0u, pir_op_val(c0, t8), pir_op_val(c1, t8), ln, lc, PIR_INVALID_ID);

                                PirInst br = pir_make_inst(PIR_OP_BR, tvoid, ln, lc);
                                br.operand_count = 1u; br.operands = pir_alloc_ops(mod, 1u);
                                br.operands[0] = pir_op_val(ceq, t8);
                                br.targets[0]  = sw_copy.switch_cases[c].target;
                                if (c + 1u < sw_copy.switch_case_count) {
                                    PirBlockId next_b = fn->blocks.len;
                                    PirBlock nb;
                                    memset(&nb, 0, sizeof(nb));
                                    char sbuf[48];
                                    snprintf(sbuf, sizeof(sbuf), "sw128_%u_%u", (unsigned)b, (unsigned)next_b);
                                    nb.label    = pir_arena_strdup(&mod->arena, sbuf);
                                    nb.src_line = ln;
                                    PIR_VEC_PUSH(&fn->blocks, nb);
                                    cur_out = (cur_blk == b) ? &new_insts : &fn->blocks.data[cur_blk].insts;
                                    br.targets[1].target = next_b;
                                    pir_fn_append_inst(fn, cur_out, br);
                                    cur_blk = next_b;
                                    cur_out = &fn->blocks.data[next_b].insts;
                                } else {
                                    br.targets[1] = sw_copy.targets[0];
                                    pir_fn_append_inst(fn, cur_out, br);
                                }
                            }
                        }
                        continue;
                    }
                }
                PIR_VEC_PUSH(&new_insts, iid);
                continue;
            }

            /* Check trunc from i128 -> narrower int */
            if (copy.op == PIR_OP_TRUNC && copy.operand_count == 1u &&
                is_i128(copy.operands[0].type)) {
                PirOperand lo, hi;
                operand_to_pair(&copy.operands[0], map, &lo, &hi);
                if (copy.ty.u.scalar == PIR_SCALAR_I64 && lo.kind == PIR_OPER_VAL) {
                    ValAlias al = { copy.results[0], lo.u.val };
                    PIR_VEC_PUSH(&aliases, al);
                } else if (copy.ty.u.scalar == PIR_SCALAR_I64) {
                    emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t64, lo, ln, lc, copy.results[0]);
                } else {
                    emit_un(mod, fn, b, &new_insts, PIR_OP_TRUNC, copy.ty, lo, ln, lc, copy.results[0]);
                }
                continue;
            }

            /* Check sitofp / uitofp from i128 -> f32/f64 (single-rounding IEEE-754) */
            if ((copy.op == PIR_OP_SITOFP || copy.op == PIR_OP_UITOFP) &&
                copy.operand_count == 1u && is_i128(copy.operands[0].type)) {
                PirType dst_ty = copy.ty;
                bool dst_f32 = (dst_ty.kind == PIR_TYPE_SCALAR && dst_ty.u.scalar == PIR_SCALAR_F32);
                PirOperand alo, ahi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                PirOperand ulo = alo, uhi = ahi;
                PirValueId is_neg = PIR_INVALID_ID;
                if (copy.op == PIR_OP_SITOFP) {
                    is_neg = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t64, 0u,
                                      ahi, pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId nlo = PIR_INVALID_ID, nhi = PIR_INVALID_ID;
                    emit_neg128(mod, fn, b, &new_insts, alo, ahi, ln, lc, &nlo, &nhi);
                    PirValueId slo = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(is_neg, t8),
                                              pir_op_val(nlo, t64), alo, ln, lc, PIR_INVALID_ID);
                    PirValueId shi = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(is_neg, t8),
                                              pir_op_val(nhi, t64), ahi, ln, lc, PIR_INVALID_ID);
                    ulo = pir_op_val(slo, t64);
                    uhi = pir_op_val(shi, t64);
                }
                PirValueId hi_z = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u,
                                           uhi, pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                PirValueId lz = emit_un(mod, fn, b, &new_insts, PIR_OP_CLZ, t64, uhi, ln, lc, PIR_INVALID_ID);
                PirValueId lz_z = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u,
                                           pir_op_val(lz, t64), pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                PirValueId sh_r = emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t64, 0u,
                                           pir_op_imm_i(64u, false, t64), pir_op_val(lz, t64), ln, lc, PIR_INVALID_ID);
                PirValueId hi_shl = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t64, 0u,
                                             uhi, pir_op_val(lz, t64), ln, lc, PIR_INVALID_ID);
                PirValueId lo_shr = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHR, t64, 0u,
                                             ulo, pir_op_val(sh_r, t64), ln, lc, PIR_INVALID_ID);
                PirValueId lo_top = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(lz_z, t8),
                                             pir_op_imm_i(0u, false, t64), pir_op_val(lo_shr, t64), ln, lc, PIR_INVALID_ID);
                PirValueId top64 = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t64, 0u,
                                            pir_op_val(hi_shl, t64), pir_op_val(lo_top, t64), ln, lc, PIR_INVALID_ID);
                PirValueId lo_rem = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t64, 0u,
                                             ulo, pir_op_val(lz, t64), ln, lc, PIR_INVALID_ID);
                PirValueId rem64 = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(lz_z, t8),
                                            ulo, pir_op_val(lo_rem, t64), ln, lc, PIR_INVALID_ID);
                PirValueId has_st = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t64, 0u,
                                             pir_op_val(rem64, t64), pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                PirValueId st64 = emit_un(mod, fn, b, &new_insts, PIR_OP_ZEXT, t64,
                                          pir_op_val(has_st, t8), ln, lc, PIR_INVALID_ID);
                PirValueId mant64 = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t64, 0u,
                                             pir_op_val(top64, t64), pir_op_val(st64, t64), ln, lc, PIR_INVALID_ID);
                PirValueId lo_f = emit_un(mod, fn, b, &new_insts, PIR_OP_UITOFP, dst_ty, ulo, ln, lc, PIR_INVALID_ID);
                PirValueId mant_f = emit_un(mod, fn, b, &new_insts, PIR_OP_UITOFP, dst_ty,
                                            pir_op_val(mant64, t64), ln, lc, PIR_INVALID_ID);
                PirValueId pow2_f = PIR_INVALID_ID;
                if (dst_f32) {
                    PirType t32 = pir_type_scalar(PIR_SCALAR_I32);
                    PirValueId exp64 = emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t64, 0u,
                                                pir_op_imm_i(191u, false, t64), pir_op_val(lz, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId exp32 = emit_un(mod, fn, b, &new_insts, PIR_OP_TRUNC, t32,
                                               pir_op_val(exp64, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId fbits = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t32, 0u,
                                                pir_op_val(exp32, t32), pir_op_imm_i(23u, false, t32), ln, lc, PIR_INVALID_ID);
                    pow2_f = emit_un(mod, fn, b, &new_insts, PIR_OP_BITCAST, dst_ty,
                                     pir_op_val(fbits, t32), ln, lc, PIR_INVALID_ID);
                } else {
                    PirValueId exp64 = emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t64, 0u,
                                                pir_op_imm_i(1087u, false, t64), pir_op_val(lz, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId fbits = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t64, 0u,
                                                pir_op_val(exp64, t64), pir_op_imm_i(52u, false, t64), ln, lc, PIR_INVALID_ID);
                    pow2_f = emit_un(mod, fn, b, &new_insts, PIR_OP_BITCAST, dst_ty,
                                     pir_op_val(fbits, t64), ln, lc, PIR_INVALID_ID);
                }
                PirValueId hi_f = emit_bin(mod, fn, b, &new_insts, PIR_OP_FMUL, dst_ty, 0u,
                                           pir_op_val(mant_f, dst_ty), pir_op_val(pow2_f, dst_ty), ln, lc, PIR_INVALID_ID);
                PirValueId mag_dst = (copy.op == PIR_OP_UITOFP) ? copy.results[0] : PIR_INVALID_ID;
                PirValueId mag_f = emit_sel(mod, fn, b, &new_insts, dst_ty, pir_op_val(hi_z, t8),
                                            pir_op_val(lo_f, dst_ty), pir_op_val(hi_f, dst_ty), ln, lc, mag_dst);
                if (copy.op == PIR_OP_SITOFP) {
                    PirValueId nf = emit_un(mod, fn, b, &new_insts, PIR_OP_FNEG, dst_ty,
                                            pir_op_val(mag_f, dst_ty), ln, lc, PIR_INVALID_ID);
                    emit_sel(mod, fn, b, &new_insts, dst_ty, pir_op_val(is_neg, t8),
                             pir_op_val(nf, dst_ty), pir_op_val(mag_f, dst_ty), ln, lc, copy.results[0]);
                }
                continue;
            }

            /* Check bitcast from i128 -> 128-bit vector */
            if (copy.op == PIR_OP_BITCAST && copy.operand_count == 1u &&
                is_i128(copy.operands[0].type) && !is_i128(copy.ty)) {
                PirOperand vlo, vhi;
                operand_to_pair(&copy.operands[0], map, &vlo, &vhi);
                PirInst al = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, ln, lc);
                al.imm_u64 = 16u; al.align = 16u;
                al.results[0] = pir_fn_new_val(mod, fn, "bc128", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, al);

                PirInst s0 = pir_make_inst(PIR_OP_STORE, t64, ln, lc);
                s0.align = 8u; s0.operand_count = 2u; s0.operands = pir_alloc_ops(mod, 2u);
                s0.operands[0] = target->is_big_endian ? vhi : vlo;
                s0.operands[1] = pir_op_val(al.results[0], tptr);
                pir_fn_append_inst(fn, &new_insts, s0);

                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = pir_op_val(al.results[0], tptr);
                pa.operands[1] = pir_op_imm_i(8u, false, idx_ty);
                pa.results[0]  = pir_fn_new_val(mod, fn, "bc128p", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);

                PirInst s1 = pir_make_inst(PIR_OP_STORE, t64, ln, lc);
                s1.align = 8u; s1.operand_count = 2u; s1.operands = pir_alloc_ops(mod, 2u);
                s1.operands[0] = target->is_big_endian ? vlo : vhi;
                s1.operands[1] = pir_op_val(pa.results[0], tptr);
                pir_fn_append_inst(fn, &new_insts, s1);

                PirInst ld = pir_make_inst(PIR_OP_LOAD, copy.ty, ln, lc);
                ld.align = 16u; ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
                ld.operands[0] = pir_op_val(al.results[0], tptr);
                ld.results[0]  = copy.results[0];
                pir_fn_append_inst(fn, &new_insts, ld);
                continue;
            }

            if (!is_i128(copy.ty)) {
                PIR_VEC_PUSH(&new_insts, iid);
                continue;
            }

            PirValueId rlo = (copy.results[0] != PIR_INVALID_ID && copy.results[0] < orig_vals)
                             ? map[copy.results[0]].lo : PIR_INVALID_ID;
            PirValueId rhi = (copy.results[0] != PIR_INVALID_ID && copy.results[0] < orig_vals)
                             ? map[copy.results[0]].hi : PIR_INVALID_ID;

            if (copy.op == PIR_OP_CONST) {
                uint64_t lo = 0u, hi = 0u;
                split_i128_imm(&copy.operands[0], &lo, &hi);
                emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t64, pir_op_imm_i(lo, false, t64), ln, lc, rlo);
                emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t64, pir_op_imm_i(hi, false, t64), ln, lc, rhi);
            } else if (copy.op == PIR_OP_ZEXT) {
                PirOperand src = copy.operands[0];
                if (src.type.u.scalar == PIR_SCALAR_I64) {
                    if (src.kind == PIR_OPER_VAL) {
                        ValAlias al = { rlo, src.u.val };
                        PIR_VEC_PUSH(&aliases, al);
                    } else {
                        emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t64, src, ln, lc, rlo);
                    }
                } else {
                    emit_un(mod, fn, b, &new_insts, PIR_OP_ZEXT, t64, src, ln, lc, rlo);
                }
                emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t64, pir_op_imm_i(0u, false, t64), ln, lc, rhi);
            } else if (copy.op == PIR_OP_SEXT) {
                PirOperand src = copy.operands[0];
                PirOperand lo_op;
                if (src.type.u.scalar == PIR_SCALAR_I64) {
                    if (src.kind == PIR_OPER_VAL) {
                        ValAlias al = { rlo, src.u.val };
                        PIR_VEC_PUSH(&aliases, al);
                        lo_op = src;
                    } else {
                        emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t64, src, ln, lc, rlo);
                        lo_op = pir_op_val(rlo, t64);
                    }
                } else {
                    emit_un(mod, fn, b, &new_insts, PIR_OP_SEXT, t64, src, ln, lc, rlo);
                    lo_op = pir_op_val(rlo, t64);
                }
                emit_bin(mod, fn, b, &new_insts, PIR_OP_SAR, t64, PIR_FLAG_MASK,
                         lo_op, pir_op_imm_i(63u, false, t64), ln, lc, rhi);
            } else if (copy.op == PIR_OP_LOAD) {
                uint32_t al = copy.align < 8u ? copy.align : 8u;
                PirInst l0 = pir_make_inst(PIR_OP_LOAD, t64, ln, lc);
                l0.align = al; l0.flags = copy.flags;
                l0.operand_count = 1u; l0.operands = pir_alloc_ops(mod, 1u);
                l0.operands[0] = copy.operands[0];
                l0.results[0] = target->is_big_endian ? rhi : rlo;
                pir_fn_append_inst(fn, &new_insts, l0);

                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS;
                pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = copy.operands[0];
                pa.operands[1] = pir_op_imm_i(8u, false, idx_ty);
                pa.results[0] = pir_fn_new_val(mod, fn, "p128", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);

                PirInst l1 = pir_make_inst(PIR_OP_LOAD, t64, ln, lc);
                l1.align = al; l1.flags = copy.flags;
                l1.operand_count = 1u; l1.operands = pir_alloc_ops(mod, 1u);
                l1.operands[0] = pir_op_val(pa.results[0], tptr);
                l1.results[0] = target->is_big_endian ? rlo : rhi;
                pir_fn_append_inst(fn, &new_insts, l1);
            } else if (copy.op == PIR_OP_STORE) {
                PirOperand vlo, vhi;
                operand_to_pair(&copy.operands[0], map, &vlo, &vhi);
                uint32_t al = copy.align < 8u ? copy.align : 8u;
                PirInst s0 = pir_make_inst(PIR_OP_STORE, t64, ln, lc);
                s0.align = al; s0.flags = copy.flags;
                s0.operand_count = 2u; s0.operands = pir_alloc_ops(mod, 2u);
                s0.operands[0] = target->is_big_endian ? vhi : vlo;
                s0.operands[1] = copy.operands[1];
                pir_fn_append_inst(fn, &new_insts, s0);

                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS;
                pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = copy.operands[1];
                pa.operands[1] = pir_op_imm_i(8u, false, idx_ty);
                pa.results[0] = pir_fn_new_val(mod, fn, "p128", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);

                PirInst s1 = pir_make_inst(PIR_OP_STORE, t64, ln, lc);
                s1.align = al; s1.flags = copy.flags;
                s1.operand_count = 2u; s1.operands = pir_alloc_ops(mod, 2u);
                s1.operands[0] = target->is_big_endian ? vlo : vhi;
                s1.operands[1] = pir_op_val(pa.results[0], tptr);
                pir_fn_append_inst(fn, &new_insts, s1);
            } else if (copy.op == PIR_OP_ADD || copy.op == PIR_OP_SUB) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair(&copy.operands[1], map, &blo, &bhi);
                PirOpcode cop = (copy.op == PIR_OP_ADD) ? PIR_OP_ADD_CARRY : PIR_OP_SUB_BORROW;
                PirInst c0 = pir_make_inst(cop, t64, ln, lc);
                c0.operand_count = 3u; c0.operands = pir_alloc_ops(mod, 3u);
                c0.operands[0] = alo; c0.operands[1] = blo; c0.operands[2] = pir_op_imm_i(0u, false, t8);
                c0.results[0]  = rlo;
                c0.results[1]  = pir_fn_new_val(mod, fn, "cy", t8, b, PIR_INVALID_ID, 1u);
                pir_fn_append_inst(fn, &new_insts, c0);

                PirInst c1 = pir_make_inst(cop, t64, ln, lc);
                c1.operand_count = 3u; c1.operands = pir_alloc_ops(mod, 3u);
                c1.operands[0] = ahi; c1.operands[1] = bhi; c1.operands[2] = pir_op_val(c0.results[1], t8);
                c1.results[0]  = rhi;
                c1.results[1]  = pir_fn_new_val(mod, fn, "cyo", t8, b, PIR_INVALID_ID, 1u);
                pir_fn_append_inst(fn, &new_insts, c1);

                if (copy.flags & PIR_FLAG_TRAP_U) {
                    emit_trap_if(mod, fn, b, &new_insts, c1.results[1], ln, lc);
                } else if (copy.flags & PIR_FLAG_TRAP_S) {
                    PirValueId x1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t64, 0u,
                                             ahi, (copy.op == PIR_OP_ADD) ? pir_op_val(rhi, t64) : bhi,
                                             ln, lc, PIR_INVALID_ID);
                    PirValueId x2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t64, 0u,
                                             (copy.op == PIR_OP_ADD) ? bhi : ahi, pir_op_val(rhi, t64),
                                             ln, lc, PIR_INVALID_ID);
                    PirValueId an = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t64, 0u,
                                             pir_op_val(x1, t64), pir_op_val(x2, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId ov = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t64, 0u,
                                             pir_op_val(an, t64), pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                    emit_trap_if(mod, fn, b, &new_insts, ov, ln, lc);
                }
            } else if (copy.op == PIR_OP_ADD_CARRY || copy.op == PIR_OP_SUB_BORROW) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair(&copy.operands[1], map, &blo, &bhi);
                PirInst c0 = pir_make_inst(copy.op, t64, ln, lc);
                c0.operand_count = 3u; c0.operands = pir_alloc_ops(mod, 3u);
                c0.operands[0] = alo; c0.operands[1] = blo; c0.operands[2] = copy.operands[2];
                c0.results[0]  = rlo;
                c0.results[1]  = pir_fn_new_val(mod, fn, "cy", t8, b, PIR_INVALID_ID, 1u);
                pir_fn_append_inst(fn, &new_insts, c0);

                PirInst c1 = pir_make_inst(copy.op, t64, ln, lc);
                c1.operand_count = 3u; c1.operands = pir_alloc_ops(mod, 3u);
                c1.operands[0] = ahi; c1.operands[1] = bhi; c1.operands[2] = pir_op_val(c0.results[1], t8);
                c1.results[0]  = rhi;
                c1.results[1]  = copy.results[1];
                pir_fn_append_inst(fn, &new_insts, c1);
            } else if (copy.op == PIR_OP_ADD_OVF_U || copy.op == PIR_OP_SUB_OVF_U ||
                       copy.op == PIR_OP_ADD_OVF_S || copy.op == PIR_OP_SUB_OVF_S) {
                bool is_add = (copy.op == PIR_OP_ADD_OVF_U || copy.op == PIR_OP_ADD_OVF_S);
                bool is_s   = (copy.op == PIR_OP_ADD_OVF_S || copy.op == PIR_OP_SUB_OVF_S);
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair(&copy.operands[1], map, &blo, &bhi);
                PirOpcode cop = is_add ? PIR_OP_ADD_CARRY : PIR_OP_SUB_BORROW;
                PirInst c0 = pir_make_inst(cop, t64, ln, lc);
                c0.operand_count = 3u; c0.operands = pir_alloc_ops(mod, 3u);
                c0.operands[0] = alo; c0.operands[1] = blo; c0.operands[2] = pir_op_imm_i(0u, false, t8);
                c0.results[0]  = rlo;
                c0.results[1]  = pir_fn_new_val(mod, fn, "cy", t8, b, PIR_INVALID_ID, 1u);
                pir_fn_append_inst(fn, &new_insts, c0);

                PirInst c1 = pir_make_inst(cop, t64, ln, lc);
                c1.operand_count = 3u; c1.operands = pir_alloc_ops(mod, 3u);
                c1.operands[0] = ahi; c1.operands[1] = bhi; c1.operands[2] = pir_op_val(c0.results[1], t8);
                c1.results[0]  = rhi;
                c1.results[1]  = is_s ? pir_fn_new_val(mod, fn, "cyo", t8, b, PIR_INVALID_ID, 1u)
                                      : copy.results[1];
                pir_fn_append_inst(fn, &new_insts, c1);
                if (is_s) {
                    PirValueId x1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t64, 0u,
                                             ahi, is_add ? pir_op_val(rhi, t64) : bhi,
                                             ln, lc, PIR_INVALID_ID);
                    PirValueId x2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t64, 0u,
                                             is_add ? bhi : ahi, pir_op_val(rhi, t64),
                                             ln, lc, PIR_INVALID_ID);
                    PirValueId an = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t64, 0u,
                                             pir_op_val(x1, t64), pir_op_val(x2, t64), ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t64, 0u,
                             pir_op_val(an, t64), pir_op_imm_i(0u, false, t64), ln, lc, copy.results[1]);
                }
            } else if (copy.op == PIR_OP_MUL || copy.op == PIR_OP_MUL_OVF_U || copy.op == PIR_OP_MUL_OVF_S) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair(&copy.operands[1], map, &blo, &bhi);
                bool need_ovf_u = (copy.op == PIR_OP_MUL_OVF_U) || (copy.op == PIR_OP_MUL && (copy.flags & PIR_FLAG_TRAP_U));
                bool need_ovf_s = (copy.op == PIR_OP_MUL_OVF_S) || (copy.op == PIR_OP_MUL && (copy.flags & PIR_FLAG_TRAP_S));
                if (need_ovf_u) {
                    PirValueId ov = emit_mul_ovf_u128(mod, fn, b, &new_insts, alo, ahi, blo, bhi, ln, lc, rlo, rhi);
                    if (copy.op == PIR_OP_MUL_OVF_U) {
                        ValAlias al = { copy.results[1], ov };
                        PIR_VEC_PUSH(&aliases, al);
                    } else {
                        emit_trap_if(mod, fn, b, &new_insts, ov, ln, lc);
                    }
                } else if (need_ovf_s) {
                    PirValueId na = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t64, 0u, ahi, pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId nb = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t64, 0u, bhi, pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId nr = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t8, 0u, pir_op_val(na, t8), pir_op_val(nb, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId nalo = PIR_INVALID_ID, nahi = PIR_INVALID_ID;
                    PirValueId nblo = PIR_INVALID_ID, nbhi = PIR_INVALID_ID;
                    emit_neg128(mod, fn, b, &new_insts, alo, ahi, ln, lc, &nalo, &nahi);
                    emit_neg128(mod, fn, b, &new_insts, blo, bhi, ln, lc, &nblo, &nbhi);
                    PirValueId ualo = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(na, t8), pir_op_val(nalo, t64), alo, ln, lc, PIR_INVALID_ID);
                    PirValueId uahi = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(na, t8), pir_op_val(nahi, t64), ahi, ln, lc, PIR_INVALID_ID);
                    PirValueId ublo = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(nb, t8), pir_op_val(nblo, t64), blo, ln, lc, PIR_INVALID_ID);
                    PirValueId ubhi = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(nb, t8), pir_op_val(nbhi, t64), bhi, ln, lc, PIR_INVALID_ID);
                    PirValueId plo = pir_fn_new_val(mod, fn, "plo", t64, b, PIR_INVALID_ID, 0u);
                    PirValueId phi = pir_fn_new_val(mod, fn, "phi", t64, b, PIR_INVALID_ID, 0u);
                    PirValueId uov = emit_mul_ovf_u128(mod, fn, b, &new_insts,
                                                       pir_op_val(ualo, t64), pir_op_val(uahi, t64),
                                                       pir_op_val(ublo, t64), pir_op_val(ubhi, t64),
                                                       ln, lc, plo, phi);
                    PirValueId nplo = PIR_INVALID_ID, nphi = PIR_INVALID_ID;
                    emit_neg128(mod, fn, b, &new_insts, pir_op_val(plo, t64), pir_op_val(phi, t64), ln, lc, &nplo, &nphi);
                    emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(nr, t8), pir_op_val(nplo, t64), pir_op_val(plo, t64), ln, lc, rlo);
                    emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(nr, t8), pir_op_val(nphi, t64), pir_op_val(phi, t64), ln, lc, rhi);
                    /* Signed magnitude fit: if !nr, phi <= INT64_MAX; if nr, (phi < 2^63) || (phi == 2^63 && plo == 0). */
                    PirValueId pos_bad = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_GT_U, t64, 0u, pir_op_val(phi, t64), pir_op_imm_i((uint64_t)INT64_MAX, false, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId neg_gt  = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_GT_U, t64, 0u, pir_op_val(phi, t64), UINT64_C(0x8000000000000000) ? pir_op_imm_i(UINT64_C(0x8000000000000000), false, t64) : pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId neg_eq  = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, pir_op_val(phi, t64), pir_op_imm_i(UINT64_C(0x8000000000000000), false, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId plo_nz  = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t64, 0u, pir_op_val(plo, t64), pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId neg_e2  = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(neg_eq, t8), pir_op_val(plo_nz, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId neg_bad = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(neg_gt, t8), pir_op_val(neg_e2, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId mag_bad = emit_sel(mod, fn, b, &new_insts, t8, pir_op_val(nr, t8), pir_op_val(neg_bad, t8), pir_op_val(pos_bad, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId ov = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(uov, t8), pir_op_val(mag_bad, t8), ln, lc,
                                             (copy.op == PIR_OP_MUL_OVF_S) ? copy.results[1] : PIR_INVALID_ID);
                    if (copy.op == PIR_OP_MUL) {
                        emit_trap_if(mod, fn, b, &new_insts, ov, ln, lc);
                    }
                } else {
                    PirInst mw = pir_make_inst(PIR_OP_MUL_WIDE_U, t64, ln, lc);
                    mw.operand_count = 2u; mw.operands = pir_alloc_ops(mod, 2u);
                    mw.operands[0] = alo; mw.operands[1] = blo;
                    mw.results[0]  = rlo;
                    mw.results[1]  = pir_fn_new_val(mod, fn, "mhi", t64, b, PIR_INVALID_ID, 1u);
                    pir_fn_append_inst(fn, &new_insts, mw);
                    PirValueId c1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_MUL, t64, PIR_FLAG_WRAP, alo, bhi, ln, lc, PIR_INVALID_ID);
                    PirValueId c2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_MUL, t64, PIR_FLAG_WRAP, ahi, blo, ln, lc, PIR_INVALID_ID);
                    PirValueId t0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_ADD, t64, PIR_FLAG_WRAP, pir_op_val(mw.results[1], t64), pir_op_val(c1, t64), ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_ADD, t64, PIR_FLAG_WRAP, pir_op_val(t0, t64), pir_op_val(c2, t64), ln, lc, rhi);
                }
            } else if (copy.op == PIR_OP_AND || copy.op == PIR_OP_OR || copy.op == PIR_OP_XOR) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair(&copy.operands[1], map, &blo, &bhi);
                emit_bin(mod, fn, b, &new_insts, copy.op, t64, PIR_FLAG_NONE, alo, blo, ln, lc, rlo);
                emit_bin(mod, fn, b, &new_insts, copy.op, t64, PIR_FLAG_NONE, ahi, bhi, ln, lc, rhi);
            } else if (copy.op == PIR_OP_SHL || copy.op == PIR_OP_SHR || copy.op == PIR_OP_SAR) {
                PirOperand alo, ahi, slo, shi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair(&copy.operands[1], map, &slo, &shi);
                if (copy.flags & PIR_FLAG_TRAP) {
                    PirValueId hi_nz = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t64, 0u, shi, pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId lo_ge = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_GE_U, t64, 0u, slo, pir_op_imm_i(128u, false, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId bad   = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(hi_nz, t8), pir_op_val(lo_ge, t8), ln, lc, PIR_INVALID_ID);
                    emit_trap_if(mod, fn, b, &new_insts, bad, ln, lc);
                }
                PirValueId s   = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t64, 0u, slo, pir_op_imm_i(127u, false, t64), ln, lc, PIR_INVALID_ID);
                PirValueId s64 = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t64, 0u, pir_op_val(s, t64), pir_op_imm_i(63u, false, t64), ln, lc, PIR_INVALID_ID);
                PirValueId lt  = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_U, t64, 0u, pir_op_val(s, t64), pir_op_imm_i(64u, false, t64), ln, lc, PIR_INVALID_ID);
                PirValueId is0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, pir_op_val(s64, t64), pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                PirValueId inv = emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t64, 0u, pir_op_imm_i(64u, false, t64), pir_op_val(s64, t64), ln, lc, PIR_INVALID_ID);
                if (copy.op == PIR_OP_SHL) {
                    PirValueId lo_s = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t64, PIR_FLAG_MASK, alo, pir_op_val(s64, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId c_nz = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHR, t64, PIR_FLAG_MASK, alo, pir_op_val(inv, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId cy   = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(is0, t8), pir_op_imm_i(0u, false, t64), pir_op_val(c_nz, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId hsh  = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t64, PIR_FLAG_MASK, ahi, pir_op_val(s64, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId hi_s = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t64, 0u, pir_op_val(hsh, t64), pir_op_val(cy, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId hi_b = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t64, PIR_FLAG_MASK, alo, pir_op_val(s64, t64), ln, lc, PIR_INVALID_ID);
                    emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(lt, t8), pir_op_val(lo_s, t64), pir_op_imm_i(0u, false, t64), ln, lc, rlo);
                    emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(lt, t8), pir_op_val(hi_s, t64), pir_op_val(hi_b, t64), ln, lc, rhi);
                } else {
                    PirOpcode hi_op = (copy.op == PIR_OP_SAR) ? PIR_OP_SAR : PIR_OP_SHR;
                    PirValueId lsh  = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHR, t64, PIR_FLAG_MASK, alo, pir_op_val(s64, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId c_nz = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t64, PIR_FLAG_MASK, ahi, pir_op_val(inv, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId cy   = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(is0, t8), pir_op_imm_i(0u, false, t64), pir_op_val(c_nz, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId lo_s = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t64, 0u, pir_op_val(lsh, t64), pir_op_val(cy, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId hi_s = emit_bin(mod, fn, b, &new_insts, hi_op, t64, PIR_FLAG_MASK, ahi, pir_op_val(s64, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId lo_b = emit_bin(mod, fn, b, &new_insts, hi_op, t64, PIR_FLAG_MASK, ahi, pir_op_val(s64, t64), ln, lc, PIR_INVALID_ID);
                    PirOperand hi_b = (copy.op == PIR_OP_SAR)
                                      ? pir_op_val(emit_bin(mod, fn, b, &new_insts, PIR_OP_SAR, t64, PIR_FLAG_MASK, ahi, pir_op_imm_i(63u, false, t64), ln, lc, PIR_INVALID_ID), t64)
                                      : pir_op_imm_i(0u, false, t64);
                    emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(lt, t8), pir_op_val(lo_s, t64), pir_op_val(lo_b, t64), ln, lc, rlo);
                    emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(lt, t8), pir_op_val(hi_s, t64), hi_b, ln, lc, rhi);
                }
            } else if (copy.op == PIR_OP_DIV_S || copy.op == PIR_OP_DIV_U ||
                       copy.op == PIR_OP_REM_S || copy.op == PIR_OP_REM_U) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair(&copy.operands[1], map, &blo, &bhi);
                bool is_s = (copy.op == PIR_OP_DIV_S || copy.op == PIR_OP_REM_S);
                if ((copy.flags & PIR_FLAG_UNSAFE) == 0u) {
                    PirValueId bor = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t64, 0u, blo, bhi, ln, lc, PIR_INVALID_ID);
                    PirValueId b0  = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, pir_op_val(bor, t64), pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                    emit_trap_if(mod, fn, b, &new_insts, b0, ln, lc);
                    if (is_s && (copy.flags & PIR_FLAG_WRAP) == 0u) {
                        PirValueId al0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, alo, pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                        PirValueId ahm = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, ahi, pir_op_imm_i(UINT64_C(0x8000000000000000), false, t64), ln, lc, PIR_INVALID_ID);
                        PirValueId blm = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, blo, pir_op_imm_i(UINT64_MAX, false, t64), ln, lc, PIR_INVALID_ID);
                        PirValueId bhm = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, bhi, pir_op_imm_i(UINT64_MAX, false, t64), ln, lc, PIR_INVALID_ID);
                        PirValueId t1  = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(al0, t8), pir_op_val(ahm, t8), ln, lc, PIR_INVALID_ID);
                        PirValueId t2  = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(blm, t8), pir_op_val(bhm, t8), ln, lc, PIR_INVALID_ID);
                        PirValueId ovf = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(t1, t8), pir_op_val(t2, t8), ln, lc, PIR_INVALID_ID);
                        emit_trap_if(mod, fn, b, &new_insts, ovf, ln, lc);
                    }
                }
                const char *cname = (copy.op == PIR_OP_DIV_S) ? "__divti3"
                                  : (copy.op == PIR_OP_DIV_U) ? "__udivti3"
                                  : (copy.op == PIR_OP_REM_S) ? "__modti3" : "__umodti3";
                PirType ptys[5] = { tptr, t64, t64, t64, t64 };
                PirFuncId cid = pir_ensure_extern_func(mod, cname, ptys, 5u, pir_type_scalar(PIR_SCALAR_VOID));
                fn = &mod->funcs.data[fi]; /* Re-fetch after potential realloc */

                PirInst al = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, ln, lc);
                al.imm_u64 = 16u; al.align = 8u;
                al.results[0] = pir_fn_new_val(mod, fn, "ti_out", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, al);

                PirInst call = pir_make_inst(PIR_OP_CALL, pir_type_scalar(PIR_SCALAR_VOID), ln, lc);
                call.flags = PIR_FLAG_NOUNWIND;
                call.operand_count = 6u; call.operands = pir_alloc_ops(mod, 6u);
                call.operands[0] = pir_op_sym_func(mod->funcs.data[cid].name, cid);
                call.operands[1] = pir_op_val(al.results[0], tptr);
                call.operands[2] = alo; call.operands[3] = ahi;
                call.operands[4] = blo; call.operands[5] = bhi;
                pir_fn_append_inst(fn, &new_insts, call);

                PirInst l0 = pir_make_inst(PIR_OP_LOAD, t64, ln, lc);
                l0.align = 8u; l0.operand_count = 1u; l0.operands = pir_alloc_ops(mod, 1u);
                l0.operands[0] = pir_op_val(al.results[0], tptr);
                l0.results[0]  = rlo;
                pir_fn_append_inst(fn, &new_insts, l0);

                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = pir_op_val(al.results[0], tptr);
                pa.operands[1] = pir_op_imm_i(8u, false, idx_ty);
                pa.results[0]  = pir_fn_new_val(mod, fn, "ti_p1", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);

                PirInst l1 = pir_make_inst(PIR_OP_LOAD, t64, ln, lc);
                l1.align = 8u; l1.operand_count = 1u; l1.operands = pir_alloc_ops(mod, 1u);
                l1.operands[0] = pir_op_val(pa.results[0], tptr);
                l1.results[0]  = rhi;
                pir_fn_append_inst(fn, &new_insts, l1);
            } else if (copy.op >= PIR_OP_CMP_EQ && copy.op <= PIR_OP_CMP_GE_U) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair(&copy.operands[1], map, &blo, &bhi);
                PirValueId res_id = copy.results[0];
                if (copy.op == PIR_OP_CMP_EQ) {
                    PirValueId c0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, alo, blo, ln, lc, PIR_INVALID_ID);
                    PirValueId c1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, ahi, bhi, ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(c0, t8), pir_op_val(c1, t8), ln, lc, res_id);
                } else if (copy.op == PIR_OP_CMP_NE) {
                    PirValueId c0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t64, 0u, alo, blo, ln, lc, PIR_INVALID_ID);
                    PirValueId c1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t64, 0u, ahi, bhi, ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(c0, t8), pir_op_val(c1, t8), ln, lc, res_id);
                } else {
                    PirOpcode hi_op = (copy.op == PIR_OP_CMP_LT_S || copy.op == PIR_OP_CMP_LE_S) ? PIR_OP_CMP_LT_S
                                    : (copy.op == PIR_OP_CMP_GT_S || copy.op == PIR_OP_CMP_GE_S) ? PIR_OP_CMP_GT_S
                                    : (copy.op == PIR_OP_CMP_LT_U || copy.op == PIR_OP_CMP_LE_U) ? PIR_OP_CMP_LT_U
                                    : PIR_OP_CMP_GT_U;
                    PirOpcode lo_op = (copy.op == PIR_OP_CMP_LT_S || copy.op == PIR_OP_CMP_LT_U) ? PIR_OP_CMP_LT_U
                                    : (copy.op == PIR_OP_CMP_LE_S || copy.op == PIR_OP_CMP_LE_U) ? PIR_OP_CMP_LE_U
                                    : (copy.op == PIR_OP_CMP_GT_S || copy.op == PIR_OP_CMP_GT_U) ? PIR_OP_CMP_GT_U
                                    : PIR_OP_CMP_GE_U;
                    PirValueId heq = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, ahi, bhi, ln, lc, PIR_INVALID_ID);
                    PirValueId hc  = emit_bin(mod, fn, b, &new_insts, hi_op, t64, 0u, ahi, bhi, ln, lc, PIR_INVALID_ID);
                    PirValueId lc0 = emit_bin(mod, fn, b, &new_insts, lo_op, t64, 0u, alo, blo, ln, lc, PIR_INVALID_ID);
                    emit_sel(mod, fn, b, &new_insts, t8, pir_op_val(heq, t8), pir_op_val(lc0, t8), pir_op_val(hc, t8), ln, lc, res_id);
                }
            } else if (copy.op == PIR_OP_SELECT) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair(&copy.operands[1], map, &alo, &ahi);
                operand_to_pair(&copy.operands[2], map, &blo, &bhi);
                emit_sel(mod, fn, b, &new_insts, t64, copy.operands[0], alo, blo, ln, lc, rlo);
                emit_sel(mod, fn, b, &new_insts, t64, copy.operands[0], ahi, bhi, ln, lc, rhi);
            } else if (copy.op == PIR_OP_BSWAP) {
                PirOperand alo, ahi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                emit_un(mod, fn, b, &new_insts, PIR_OP_BSWAP, t64, ahi, ln, lc, rlo);
                emit_un(mod, fn, b, &new_insts, PIR_OP_BSWAP, t64, alo, ln, lc, rhi);
            } else if (copy.op == PIR_OP_POPCNT) {
                PirOperand alo, ahi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                PirValueId p0 = emit_un(mod, fn, b, &new_insts, PIR_OP_POPCNT, t64, alo, ln, lc, PIR_INVALID_ID);
                PirValueId p1 = emit_un(mod, fn, b, &new_insts, PIR_OP_POPCNT, t64, ahi, ln, lc, PIR_INVALID_ID);
                emit_bin(mod, fn, b, &new_insts, PIR_OP_ADD, t64, 0u, pir_op_val(p0, t64), pir_op_val(p1, t64), ln, lc, rlo);
                emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t64, pir_op_imm_i(0u, false, t64), ln, lc, rhi);
            } else if (copy.op == PIR_OP_CLZ || copy.op == PIR_OP_CTZ) {
                PirOperand alo, ahi;
                operand_to_pair(&copy.operands[0], map, &alo, &ahi);
                PirOperand primary = (copy.op == PIR_OP_CLZ) ? ahi : alo;
                PirOperand second  = (copy.op == PIR_OP_CLZ) ? alo : ahi;
                PirValueId pz  = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t64, 0u, primary, pir_op_imm_i(0u, false, t64), ln, lc, PIR_INVALID_ID);
                PirValueId c1  = emit_un(mod, fn, b, &new_insts, copy.op, t64, primary, ln, lc, PIR_INVALID_ID);
                PirValueId c2  = emit_un(mod, fn, b, &new_insts, copy.op, t64, second, ln, lc, PIR_INVALID_ID);
                PirValueId c2p = emit_bin(mod, fn, b, &new_insts, PIR_OP_ADD, t64, 0u, pir_op_val(c2, t64), pir_op_imm_i(64u, false, t64), ln, lc, PIR_INVALID_ID);
                emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(pz, t8), pir_op_val(c2p, t64), pir_op_val(c1, t64), ln, lc, rlo);
                emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t64, pir_op_imm_i(0u, false, t64), ln, lc, rhi);
            } else if (copy.op == PIR_OP_BITCAST) {
                PirInst al = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, ln, lc);
                al.imm_u64 = 16u; al.align = 16u;
                al.results[0] = pir_fn_new_val(mod, fn, "bc128", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, al);

                PirInst st = pir_make_inst(PIR_OP_STORE, copy.operands[0].type, ln, lc);
                st.align = 16u; st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
                st.operands[0] = copy.operands[0];
                st.operands[1] = pir_op_val(al.results[0], tptr);
                pir_fn_append_inst(fn, &new_insts, st);

                PirInst l0 = pir_make_inst(PIR_OP_LOAD, t64, ln, lc);
                l0.align = 8u; l0.operand_count = 1u; l0.operands = pir_alloc_ops(mod, 1u);
                l0.operands[0] = pir_op_val(al.results[0], tptr);
                l0.results[0]  = target->is_big_endian ? rhi : rlo;
                pir_fn_append_inst(fn, &new_insts, l0);

                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = pir_op_val(al.results[0], tptr);
                pa.operands[1] = pir_op_imm_i(8u, false, idx_ty);
                pa.results[0]  = pir_fn_new_val(mod, fn, "bc128p", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);

                PirInst l1 = pir_make_inst(PIR_OP_LOAD, t64, ln, lc);
                l1.align = 8u; l1.operand_count = 1u; l1.operands = pir_alloc_ops(mod, 1u);
                l1.operands[0] = pir_op_val(pa.results[0], tptr);
                l1.results[0]  = target->is_big_endian ? rlo : rhi;
                pir_fn_append_inst(fn, &new_insts, l1);
            } else if (copy.op == PIR_OP_FPTOSI || copy.op == PIR_OP_FPTOUI) {
                PirType tf64 = pir_type_scalar(PIR_SCALAR_F64);
                PirOperand x = copy.operands[0];
                if (x.type.kind == PIR_TYPE_SCALAR && x.type.u.scalar == PIR_SCALAR_F32) {
                    PirValueId x64 = emit_un(mod, fn, b, &new_insts, PIR_OP_FPEXT, tf64, x, ln, lc, PIR_INVALID_ID);
                    x = pir_op_val(x64, tf64);
                }
                PirValueId is_nan = emit_bin(mod, fn, b, &new_insts, PIR_OP_FCMP_UNE, tf64, 0u, x, x, ln, lc, PIR_INVALID_ID);
                if (copy.op == PIR_OP_FPTOUI) {
                    PirValueId lt0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_FCMP_OLT, tf64, 0u, x, pir_op_imm_f(0u, tf64), ln, lc, PIR_INVALID_ID);
                    PirValueId le_m1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_FCMP_OLE, tf64, 0u, x, pir_op_imm_f(0xBFF0000000000000ULL, tf64), ln, lc, PIR_INVALID_ID);
                    PirValueId ge_max = emit_bin(mod, fn, b, &new_insts, PIR_OP_FCMP_OGE, tf64, 0u, x, pir_op_imm_f(0x47F0000000000000ULL, tf64), ln, lc, PIR_INVALID_ID);
                    if (copy.flags & PIR_FLAG_TRAP) {
                        PirValueId b1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(is_nan, t8), pir_op_val(le_m1, t8), ln, lc, PIR_INVALID_ID);
                        PirValueId bad = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(b1, t8), pir_op_val(ge_max, t8), ln, lc, PIR_INVALID_ID);
                        emit_trap_if(mod, fn, b, &new_insts, bad, ln, lc);
                    }
                    PirValueId zero_c = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(is_nan, t8), pir_op_val(lt0, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId cx = emit_sel(mod, fn, b, &new_insts, tf64, pir_op_val(zero_c, t8), pir_op_imm_f(0u, tf64), x, ln, lc, PIR_INVALID_ID);
                    PirValueId q = emit_bin(mod, fn, b, &new_insts, PIR_OP_FMUL, tf64, 0u, pir_op_val(cx, tf64), pir_op_imm_f(0x3BF0000000000000ULL, tf64), ln, lc, PIR_INVALID_ID);
                    PirInst hi_i = pir_make_inst(PIR_OP_FPTOUI, t64, ln, lc);
                    hi_i.flags = PIR_FLAG_SAT; hi_i.operand_count = 1u; hi_i.operands = pir_alloc_ops(mod, 1u);
                    hi_i.operands[0] = pir_op_val(q, tf64);
                    hi_i.results[0]  = pir_fn_new_val(mod, fn, "fhi", t64, b, PIR_INVALID_ID, 0u);
                    pir_fn_append_inst(fn, &new_insts, hi_i);
                    PirValueId hf  = emit_un(mod, fn, b, &new_insts, PIR_OP_UITOFP, tf64, pir_op_val(hi_i.results[0], t64), ln, lc, PIR_INVALID_ID);
                    PirValueId hs  = emit_bin(mod, fn, b, &new_insts, PIR_OP_FMUL, tf64, 0u, pir_op_val(hf, tf64), pir_op_imm_f(0x43F0000000000000ULL, tf64), ln, lc, PIR_INVALID_ID);
                    PirValueId rem = emit_bin(mod, fn, b, &new_insts, PIR_OP_FSUB, tf64, 0u, pir_op_val(cx, tf64), pir_op_val(hs, tf64), ln, lc, PIR_INVALID_ID);
                    PirInst lo_i = pir_make_inst(PIR_OP_FPTOUI, t64, ln, lc);
                    lo_i.flags = PIR_FLAG_SAT; lo_i.operand_count = 1u; lo_i.operands = pir_alloc_ops(mod, 1u);
                    lo_i.operands[0] = pir_op_val(rem, tf64);
                    lo_i.results[0]  = pir_fn_new_val(mod, fn, "flo", t64, b, PIR_INVALID_ID, 0u);
                    pir_fn_append_inst(fn, &new_insts, lo_i);
                    emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(ge_max, t8), pir_op_imm_i(UINT64_MAX, false, t64), pir_op_val(lo_i.results[0], t64), ln, lc, rlo);
                    emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(ge_max, t8), pir_op_imm_i(UINT64_MAX, false, t64), pir_op_val(hi_i.results[0], t64), ln, lc, rhi);
                } else {
                    PirValueId too_lo = emit_bin(mod, fn, b, &new_insts, PIR_OP_FCMP_OLT, tf64, 0u, x, pir_op_imm_f(0xC7E0000000000000ULL, tf64), ln, lc, PIR_INVALID_ID);
                    PirValueId too_hi = emit_bin(mod, fn, b, &new_insts, PIR_OP_FCMP_OGE, tf64, 0u, x, pir_op_imm_f(0x47E0000000000000ULL, tf64), ln, lc, PIR_INVALID_ID);
                    if (copy.flags & PIR_FLAG_TRAP) {
                        PirValueId b1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(is_nan, t8), pir_op_val(too_lo, t8), ln, lc, PIR_INVALID_ID);
                        PirValueId bad = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(b1, t8), pir_op_val(too_hi, t8), ln, lc, PIR_INVALID_ID);
                        emit_trap_if(mod, fn, b, &new_insts, bad, ln, lc);
                    }
                    PirValueId is_neg = emit_bin(mod, fn, b, &new_insts, PIR_OP_FCMP_OLT, tf64, 0u, x, pir_op_imm_f(0u, tf64), ln, lc, PIR_INVALID_ID);
                    PirValueId nx = emit_un(mod, fn, b, &new_insts, PIR_OP_FNEG, tf64, x, ln, lc, PIR_INVALID_ID);
                    PirValueId ax0 = emit_sel(mod, fn, b, &new_insts, tf64, pir_op_val(is_neg, t8), pir_op_val(nx, tf64), x, ln, lc, PIR_INVALID_ID);
                    PirValueId ax  = emit_sel(mod, fn, b, &new_insts, tf64, pir_op_val(is_nan, t8), pir_op_imm_f(0u, tf64), pir_op_val(ax0, tf64), ln, lc, PIR_INVALID_ID);
                    PirValueId q   = emit_bin(mod, fn, b, &new_insts, PIR_OP_FMUL, tf64, 0u, pir_op_val(ax, tf64), pir_op_imm_f(0x3BF0000000000000ULL, tf64), ln, lc, PIR_INVALID_ID);
                    PirInst hi_i = pir_make_inst(PIR_OP_FPTOUI, t64, ln, lc);
                    hi_i.flags = PIR_FLAG_SAT; hi_i.operand_count = 1u; hi_i.operands = pir_alloc_ops(mod, 1u);
                    hi_i.operands[0] = pir_op_val(q, tf64);
                    hi_i.results[0]  = pir_fn_new_val(mod, fn, "fhi", t64, b, PIR_INVALID_ID, 0u);
                    pir_fn_append_inst(fn, &new_insts, hi_i);
                    PirValueId hf  = emit_un(mod, fn, b, &new_insts, PIR_OP_UITOFP, tf64, pir_op_val(hi_i.results[0], t64), ln, lc, PIR_INVALID_ID);
                    PirValueId hs  = emit_bin(mod, fn, b, &new_insts, PIR_OP_FMUL, tf64, 0u, pir_op_val(hf, tf64), pir_op_imm_f(0x43F0000000000000ULL, tf64), ln, lc, PIR_INVALID_ID);
                    PirValueId rem = emit_bin(mod, fn, b, &new_insts, PIR_OP_FSUB, tf64, 0u, pir_op_val(ax, tf64), pir_op_val(hs, tf64), ln, lc, PIR_INVALID_ID);
                    PirInst lo_i = pir_make_inst(PIR_OP_FPTOUI, t64, ln, lc);
                    lo_i.flags = PIR_FLAG_SAT; lo_i.operand_count = 1u; lo_i.operands = pir_alloc_ops(mod, 1u);
                    lo_i.operands[0] = pir_op_val(rem, tf64);
                    lo_i.results[0]  = pir_fn_new_val(mod, fn, "flo", t64, b, PIR_INVALID_ID, 0u);
                    pir_fn_append_inst(fn, &new_insts, lo_i);
                    PirValueId nlo = PIR_INVALID_ID, nhi = PIR_INVALID_ID;
                    emit_neg128(mod, fn, b, &new_insts, pir_op_val(lo_i.results[0], t64), pir_op_val(hi_i.results[0], t64), ln, lc, &nlo, &nhi);
                    PirValueId s1l = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(is_neg, t8), pir_op_val(nlo, t64), pir_op_val(lo_i.results[0], t64), ln, lc, PIR_INVALID_ID);
                    PirValueId s1h = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(is_neg, t8), pir_op_val(nhi, t64), pir_op_val(hi_i.results[0], t64), ln, lc, PIR_INVALID_ID);
                    PirValueId s2l = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(too_lo, t8), pir_op_imm_i(0u, false, t64), pir_op_val(s1l, t64), ln, lc, PIR_INVALID_ID);
                    PirValueId s2h = emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(too_lo, t8), pir_op_imm_i(UINT64_C(0x8000000000000000), false, t64), pir_op_val(s1h, t64), ln, lc, PIR_INVALID_ID);
                    emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(too_hi, t8), pir_op_imm_i(UINT64_MAX, false, t64), pir_op_val(s2l, t64), ln, lc, rlo);
                    emit_sel(mod, fn, b, &new_insts, t64, pir_op_val(too_hi, t8), pir_op_imm_i((uint64_t)INT64_MAX, false, t64), pir_op_val(s2h, t64), ln, lc, rhi);
                }
            } else {
                PIR_VEC_PUSH(&new_insts, iid);
            }
        }
        PIR_VEC_FREE(&mod->funcs.data[fi].blocks.data[b].insts);
        mod->funcs.data[fi].blocks.data[b].insts = new_insts;
    }

    fn = &mod->funcs.data[fi];
    for (uint32_t i = 0u; i < aliases.len; ++i) {
        PirValueId root = aliases.data[i].to;
        for (bool progress = true; progress; ) {
            progress = false;
            for (uint32_t k = 0u; k < aliases.len; ++k) {
                if (aliases.data[k].from == root) {
                    root = aliases.data[k].to;
                    progress = true;
                    break;
                }
            }
        }
        pir_fn_replace_val_uses(fn, aliases.data[i].from, root);
    }
    PIR_VEC_FREE(&aliases);
    free(map);
}

static void split_i128_decl(PirModule *mod, PirParamDecl **params_io, uint32_t *count_io,
                            PirType *ret_ty_io, PirCabiRetInfo *cabi_ret_io) {
    PirParamDecl *old_p = *params_io;
    uint32_t old_n      = *count_io;
    bool ret_is_i128    = is_i128(*ret_ty_io);
    bool has_any        = ret_is_i128;
    for (uint32_t i = 0u; i < old_n && !has_any; ++i) {
        if (is_i128(old_p[i].type)) { has_any = true; break; }
    }
    if (!has_any) return;
    PirType t64   = pir_type_scalar(PIR_SCALAR_I64);
    PirType tptr  = pir_type_scalar(PIR_SCALAR_PTR);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);
    PIR_VEC(PirParamDecl) np = {0};
    if (ret_is_i128) {
        PirParamDecl op;
        memset(&op, 0, sizeof(op));
        op.type     = tptr;
        op.abi_type = tvoid;
        op.val_id   = PIR_INVALID_ID;
        PIR_VEC_PUSH(&np, op);
        *ret_ty_io             = tvoid;
        cabi_ret_io->kind      = PIR_CABI_RET_MULTI_REG;
        cabi_ret_io->reg_count = 2u;
        cabi_ret_io->regs[0]   = t64;
        cabi_ret_io->regs[1]   = t64;
        cabi_ret_io->agg_size  = 16u;
        cabi_ret_io->agg_align = 8u;
    }
    for (uint32_t i = 0u; i < old_n; ++i) {
        if (is_i128(old_p[i].type)) {
            PirParamDecl p0 = old_p[i];
            PirParamDecl p1 = old_p[i];
            p0.type = t64; p1.type = t64;
            p1.abi_stack_align = 0u;
            p1.name = NULL; p1.val_id = PIR_INVALID_ID;
            PIR_VEC_PUSH(&np, p0);
            PIR_VEC_PUSH(&np, p1);
        } else {
            PIR_VEC_PUSH(&np, old_p[i]);
        }
    }
    *count_io  = np.len;
    *params_io = PIR_ARENA_NEW_ARRAY(&mod->arena, PirParamDecl, np.len);
    memcpy(*params_io, np.data, np.len * sizeof(PirParamDecl));
    PIR_VEC_FREE(&np);
}

bool pir_legalize_i128(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    (void)diag;
    uint32_t orig_funcs = mod->funcs.len;
    uint32_t orig_sigs  = mod->sigs.len;
    bool *fn_ret_i128  = (bool *)calloc(orig_funcs + 1u, sizeof(bool));
    bool *sig_ret_i128 = (bool *)calloc(orig_sigs + 1u, sizeof(bool));
    if (!fn_ret_i128 || !sig_ret_i128) abort();

    for (uint32_t fi = 0u; fi < orig_funcs; ++fi) {
        fn_ret_i128[fi] = is_i128(mod->funcs.data[fi].ret_type);
    }
    for (uint32_t si = 0u; si < orig_sigs; ++si) {
        sig_ret_i128[si] = is_i128(mod->sigs.data[si].ret_type);
    }

    for (uint32_t fi = 0u; fi < orig_funcs; ++fi) {
        if (mod->funcs.data[fi].is_defined) {
            legalize_i128_func(mod, target, fi, fn_ret_i128, sig_ret_i128, orig_funcs);
        } else {
            split_i128_decl(mod, &mod->funcs.data[fi].params, &mod->funcs.data[fi].param_count,
                            &mod->funcs.data[fi].ret_type, &mod->funcs.data[fi].cabi_ret);
        }
    }
    for (uint32_t si = 0u; si < orig_sigs; ++si) {
        split_i128_decl(mod, &mod->sigs.data[si].params, &mod->sigs.data[si].param_count,
                        &mod->sigs.data[si].ret_type, &mod->sigs.data[si].cabi_ret);
    }

    /* Flatten typed i128 globals into raw 16-byte data items. */
    for (uint32_t gi = 0u; gi < mod->globals.len; ++gi) {
        PirGlobal *g = &mod->globals.data[gi];
        if (!g->has_type || !is_i128(g->type)) continue;
        if (g->align < 16u) g->align = 16u;
        g->has_type = false;
        g->type     = pir_type_scalar(PIR_SCALAR_VOID);
        if (g->is_zeroinit || g->item_count == 0u || g->items[0].kind == PIR_GITEM_ZERO) {
            g->is_zeroinit = true;
            g->is_braced   = false;
            g->item_count  = 1u;
            g->items       = PIR_ARENA_NEW_ARRAY(&mod->arena, PirGlobalItem, 1u);
            g->items[0].kind = PIR_GITEM_ZERO;
            g->items[0].len  = 16u;
        } else if (g->items[0].kind == PIR_GITEM_INT) {
            uint64_t lo = g->items[0].int_lo;
            uint64_t hi = g->items[0].int_hi;
            if (g->items[0].int_neg) {
                uint64_t nlo = ~lo + 1u;
                uint64_t cy  = (nlo == 0u) ? 1u : 0u;
                lo = nlo;
                hi = ~hi + cy;
            }
            uint8_t *buf = PIR_ARENA_NEW_ARRAY(&mod->arena, uint8_t, 16u);
            for (uint32_t k = 0u; k < 8u; ++k) {
                uint32_t sh = target->is_big_endian ? ((7u - k) * 8u) : (k * 8u);
                buf[k]      = (uint8_t)(((target->is_big_endian ? hi : lo) >> sh) & 0xFFu);
                buf[8u + k] = (uint8_t)(((target->is_big_endian ? lo : hi) >> sh) & 0xFFu);
            }
            g->is_braced = true;
            g->item_count = 1u;
            g->items = PIR_ARENA_NEW_ARRAY(&mod->arena, PirGlobalItem, 1u);
            g->items[0].kind  = PIR_GITEM_BYTES;
            g->items[0].bytes = buf;
            g->items[0].len   = 16u;
        }
    }

    free(fn_ret_i128);
    free(sig_ret_i128);
    return true;
}
