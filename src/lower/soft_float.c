/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Reusable Soft-Float Legalization Pass (f32 -> i32, f64 -> i64).
 *
 * Triggered when `!target->caps.has_hw_fp` (e.g., AVR, soft-float ARM/RV32I).
 * Runs after optional ops legalization and BEFORE `pir_legalize_i64` / `pir_legalize_i32`,
 * so all lowered f64 (now i64) and f32 (now i32) operations, calls, returns,
 * spills, and globals are automatically lowered down to native integer width.
 *
 * Standard libgcc / compiler-rt helper conventions:
 *   f32: __addsf3, __subsf3, __mulsf3, __divsf3, __extendsfdf2,
 *        __floatsisf, __floatunsisf, __floatdisf, __floatundisf,
 *        __fixsfsi, __fixunssfsi, __fixsfdi, __fixunssfdi, __pir_fcmp_sf
 *   f64: __adddf3, __subdf3, __muldf3, __divdf3, __truncdfsf2,
 *        __floatsidf, __floatunsidf, __floatdidf, __floatundidf,
 *        __fixdfsi, __fixunsdfsi, __fixdfdi, __fixunsdfdi, __pir_fcmp_df
 *
 * __pir_fcmp_{sf,df}(a, b) -> i8:
 *   -1 (0xFF) if a < b, 0 if a == b, 1 if a > b, 2 if unordered (NaN).
 */

#include "lower/lower.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    PirValueId from;
    PirValueId to;
} SfAlias;

static bool is_f32(PirType t) {
    return t.kind == PIR_TYPE_SCALAR && t.u.scalar == PIR_SCALAR_F32;
}

static bool is_f64(PirType t) {
    return t.kind == PIR_TYPE_SCALAR && t.u.scalar == PIR_SCALAR_F64;
}

static bool is_fp(PirType t) {
    return is_f32(t) || is_f64(t);
}

static PirType map_fp_type(PirType t) {
    if (is_f32(t)) return pir_type_scalar(PIR_SCALAR_I32);
    if (is_f64(t)) return pir_type_scalar(PIR_SCALAR_I64);
    return t;
}

static PirOperand map_fp_operand(PirOperand op) {
    if (op.kind == PIR_OPER_IMM_FLOAT) {
        PirType it = map_fp_type(op.type);
        uint64_t bits = is_f32(op.type) ? (op.u.imm_float.bits & UINT32_MAX) : op.u.imm_float.bits;
        PirOperand r = pir_op_imm_i(bits, false, it);
        r.has_explicit_type   = op.has_explicit_type;
        r.call_stack_align    = op.call_stack_align;
        r.call_force_stack    = op.call_force_stack;
        r.call_no_f32_promote = false;
        r.call_group_words    = op.call_group_words;
        return r;
    }
    if (is_fp(op.type)) {
        op.type = map_fp_type(op.type);
        op.call_no_f32_promote = false;
    } else if (op.call_no_f32_promote) {
        /* Value was already rewritten from f32 -> i32 in the value table. */
        op.call_no_f32_promote = false;
    }
    return op;
}

static void map_branch_target_fp(PirBranchTarget *bt) {
    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        bt->args[k] = map_fp_operand(bt->args[k]);
    }
}

static PirValueId emit_bin(PirModule *mod, PirFunction *fn, PirBlockId b, PirInstIdVec *out,
                           PirOpcode op, PirType ty, uint32_t flags,
                           PirOperand a, PirOperand c, uint32_t ln, uint32_t lc,
                           PirValueId dst) {
    PirInst ins = pir_make_inst(op, ty, ln, lc);
    ins.flags = flags;
    ins.operand_count = 2u;
    ins.operands = pir_alloc_ops(mod, 2u);
    ins.operands[0] = a;
    ins.operands[1] = c;
    PirType rty = (op >= PIR_OP_CMP_EQ && op <= PIR_OP_CMP_GE_U)
                  ? pir_type_scalar(PIR_SCALAR_I8) : ty;
    ins.results[0] = (dst != PIR_INVALID_ID)
                     ? dst : pir_fn_new_val(mod, fn, "sf", rty, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

static PirValueId emit_un(PirModule *mod, PirFunction *fn, PirBlockId b, PirInstIdVec *out,
                          PirOpcode op, PirType ty, PirOperand a,
                          uint32_t ln, uint32_t lc, PirValueId dst) {
    PirInst ins = pir_make_inst(op, ty, ln, lc);
    ins.operand_count = 1u;
    ins.operands = pir_alloc_ops(mod, 1u);
    ins.operands[0] = a;
    ins.results[0] = (dst != PIR_INVALID_ID)
                     ? dst : pir_fn_new_val(mod, fn, "sf", ty, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

static PirValueId emit_sel(PirModule *mod, PirFunction *fn, PirBlockId b, PirInstIdVec *out,
                           PirType ty, PirOperand cond, PirOperand tv, PirOperand fv,
                           uint32_t ln, uint32_t lc, PirValueId dst) {
    PirInst ins = pir_make_inst(PIR_OP_SELECT, ty, ln, lc);
    ins.operand_count = 3u;
    ins.operands = pir_alloc_ops(mod, 3u);
    ins.operands[0] = cond;
    ins.operands[1] = tv;
    ins.operands[2] = fv;
    ins.results[0] = (dst != PIR_INVALID_ID)
                     ? dst : pir_fn_new_val(mod, fn, "sfs", ty, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

static PirValueId emit_call1(PirModule *mod, PirFuncId fi, PirBlockId b, PirInstIdVec *out,
                             const char *hname, PirType arg_ty, PirType ret_ty,
                             PirOperand a, uint32_t ln, uint32_t lc,
                             PirValueId dst, PirDiag *diag) {
    PirType ptys[1] = { arg_ty };
    PirFuncId hid = pir_ensure_verified_extern_func(mod, hname, ptys, 1u, ret_ty,
                                                    ln, "soft-float legalization", diag);
    if (hid == PIR_INVALID_ID) return PIR_INVALID_ID;
    PirFunction *fn = &mod->funcs.data[fi];
    PirInst cl = pir_make_inst(PIR_OP_CALL, ret_ty, ln, lc);
    cl.flags = PIR_FLAG_NOUNWIND;
    cl.operand_count = 2u;
    cl.operands = pir_alloc_ops(mod, 2u);
    cl.operands[0] = pir_op_sym_func(mod->funcs.data[hid].name, hid);
    cl.operands[1] = a;
    cl.results[0] = (dst != PIR_INVALID_ID)
                    ? dst : pir_fn_new_val(mod, fn, "sf1", ret_ty, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, cl);
    return cl.results[0];
}

static PirValueId emit_call2(PirModule *mod, PirFuncId fi, PirBlockId b, PirInstIdVec *out,
                             const char *hname, PirType arg_ty, PirType ret_ty,
                             PirOperand a, PirOperand c, uint32_t ln, uint32_t lc,
                             PirValueId dst, PirDiag *diag) {
    PirType ptys[2] = { arg_ty, arg_ty };
    PirFuncId hid = pir_ensure_verified_extern_func(mod, hname, ptys, 2u, ret_ty,
                                                    ln, "soft-float legalization", diag);
    if (hid == PIR_INVALID_ID) return PIR_INVALID_ID;
    PirFunction *fn = &mod->funcs.data[fi];
    PirInst cl = pir_make_inst(PIR_OP_CALL, ret_ty, ln, lc);
    cl.flags = PIR_FLAG_NOUNWIND;
    cl.operand_count = 3u;
    cl.operands = pir_alloc_ops(mod, 3u);
    cl.operands[0] = pir_op_sym_func(mod->funcs.data[hid].name, hid);
    cl.operands[1] = a;
    cl.operands[2] = c;
    cl.results[0] = (dst != PIR_INVALID_ID)
                    ? dst : pir_fn_new_val(mod, fn, "sf2", ret_ty, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, cl);
    return cl.results[0];
}

static void emit_trap_if_sf(PirModule *mod, PirFunction *fn, PirBlockId b, PirInstIdVec *out,
                            PirValueId cond, uint32_t ln, uint32_t lc) {
    PirType t8 = pir_type_scalar(PIR_SCALAR_I8);
    PirValueId z  = emit_bin(mod, fn, b, out, PIR_OP_CMP_EQ, t8, 0u,
                             pir_op_val(cond, t8), pir_op_imm_i(0u, false, t8), ln, lc, PIR_INVALID_ID);
    PirInst dv = pir_make_inst(PIR_OP_DIV_U, t8, ln, lc);
    dv.flags = PIR_FLAG_TRAP;
    dv.operand_count = 2u;
    dv.operands = pir_alloc_ops(mod, 2u);
    dv.operands[0] = pir_op_imm_i(1u, false, t8);
    dv.operands[1] = pir_op_val(z, t8);
    dv.results[0] = pir_fn_new_val(mod, fn, "sftrp", t8, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, dv);
}

static void rewrite_cabi_ret_fp(PirCabiRetInfo *cr) {
    if (cr->kind != PIR_CABI_RET_MULTI_REG) return;
    for (uint32_t k = 0u; k < cr->reg_count && k < 4u; ++k) {
        cr->regs[k] = map_fp_type(cr->regs[k]);
    }
}

bool pir_legalize_soft_float(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    if (!target || target->caps.has_hw_fp) {
        return true;
    }

    PirType t8  = pir_type_scalar(PIR_SCALAR_I8);
    PirType t32 = pir_type_scalar(PIR_SCALAR_I32);
    PirType t64 = pir_type_scalar(PIR_SCALAR_I64);

    /* Record original param/variadic info before rewriting declarations. */
    uint32_t orig_funcs = mod->funcs.len;
    uint32_t orig_sigs  = mod->sigs.len;

    /* 1. Rewrite all function & signature declarations (f32 -> i32, f64 -> i64). */
    for (uint32_t fi = 0u; fi < orig_funcs; ++fi) {
        PirFunction *fn = &mod->funcs.data[fi];
        fn->ret_type = map_fp_type(fn->ret_type);
        rewrite_cabi_ret_fp(&fn->cabi_ret);
        for (uint32_t i = 0u; i < fn->param_count; ++i) {
            fn->params[i].type = map_fp_type(fn->params[i].type);
        }
    }
    for (uint32_t si = 0u; si < orig_sigs; ++si) {
        PirSigDef *sig = &mod->sigs.data[si];
        sig->ret_type = map_fp_type(sig->ret_type);
        rewrite_cabi_ret_fp(&sig->cabi_ret);
        for (uint32_t i = 0u; i < sig->param_count; ++i) {
            sig->params[i].type = map_fp_type(sig->params[i].type);
        }
    }

    /* 2. Rewrite typed f32/f64 globals to integer bit-pattern globals (i32/i64). */
    for (uint32_t gi = 0u; gi < mod->globals.len; ++gi) {
        PirGlobal *g = &mod->globals.data[gi];
        if (!g->has_type || !is_fp(g->type)) continue;
        bool g32 = is_f32(g->type);
        g->type = g32 ? t32 : t64;
        if (g->linkage == PIR_LINK_EXTERN || g->is_zeroinit || g->item_count == 0u) continue;
        if (g->items[0].kind == PIR_GITEM_FLOAT) {
            uint64_t bits = g32 ? (g->items[0].float_bits & UINT32_MAX) : g->items[0].float_bits;
            g->items[0].kind    = PIR_GITEM_INT;
            g->items[0].int_lo  = bits;
            g->items[0].int_hi  = 0u;
            g->items[0].int_neg = false;
        } else if (g->items[0].kind == PIR_GITEM_INT) {
            uint64_t bits = 0u;
            if (g32) {
                float fv = g->items[0].int_neg ? -(float)g->items[0].int_lo : (float)g->items[0].int_lo;
                uint32_t fb = 0u;
                memcpy(&fb, &fv, 4u);
                bits = (uint64_t)fb;
            } else {
                double dv = g->items[0].int_neg ? -(double)g->items[0].int_lo : (double)g->items[0].int_lo;
                memcpy(&bits, &dv, 8u);
            }
            g->items[0].int_lo  = bits;
            g->items[0].int_hi  = 0u;
            g->items[0].int_neg = false;
        }
    }

    /* 3. Rewrite all defined functions. */
    for (uint32_t fi = 0u; fi < orig_funcs; ++fi) {
        if (!mod->funcs.data[fi].is_defined) continue;

        /* Track which values were originally f32 / f64 before rewriting their types. */
        uint32_t orig_vals = mod->funcs.data[fi].values.len;
        uint8_t *val_fp_kind = (uint8_t *)calloc(orig_vals + 1u, 1u);
        if (!val_fp_kind) abort();
        for (uint32_t v = 0u; v < orig_vals; ++v) {
            PirType vt = mod->funcs.data[fi].values.data[v].type;
            if (is_f32(vt)) {
                val_fp_kind[v] = 1u;
                mod->funcs.data[fi].values.data[v].type = t32;
            } else if (is_f64(vt)) {
                val_fp_kind[v] = 2u;
                mod->funcs.data[fi].values.data[v].type = t64;
            }
        }

        PIR_VEC(SfAlias) aliases = {0};

        for (uint32_t b = 0u; b < mod->funcs.data[fi].blocks.len; ++b) {
            PirInstIdVec new_insts = {0};
            uint32_t old_len = mod->funcs.data[fi].blocks.data[b].insts.len;
            for (uint32_t j = 0u; j < old_len; ++j) {
                PirInstId iid = mod->funcs.data[fi].blocks.data[b].insts.data[j];
                PirInst copy  = mod->funcs.data[fi].insts.data[iid];
                uint32_t ln = copy.src_line, lc = copy.loc_idx;

                if (copy.op == PIR_OP_JMP || copy.op == PIR_OP_BR || copy.op == PIR_OP_SWITCH) {
                    map_branch_target_fp(&mod->funcs.data[fi].insts.data[iid].targets[0]);
                    if (copy.op == PIR_OP_BR) {
                        map_branch_target_fp(&mod->funcs.data[fi].insts.data[iid].targets[1]);
                    } else if (copy.op == PIR_OP_SWITCH) {
                        for (uint32_t c = 0u; c < copy.switch_case_count; ++c) {
                            map_branch_target_fp(&mod->funcs.data[fi].insts.data[iid].switch_cases[c].target);
                        }
                    }
                    PIR_VEC_PUSH(&new_insts, iid);
                    continue;
                }

                if (copy.op == PIR_OP_CALL || copy.op == PIR_OP_CALL_INDIRECT) {
                    bool is_var = false;
                    uint32_t fixed_n = 0u;
                    if (copy.op == PIR_OP_CALL && copy.operands[0].u.sym.is_func) {
                        const PirFunction *cf = &mod->funcs.data[copy.operands[0].u.sym.id];
                        is_var = cf->is_variadic;
                        fixed_n = cf->param_count;
                    } else if (copy.op == PIR_OP_CALL_INDIRECT && copy.sig_id < mod->sigs.len) {
                        const PirSigDef *cs = &mod->sigs.data[copy.sig_id];
                        is_var = cs->is_variadic;
                        fixed_n = cs->param_count;
                    }
                    PirOperand *nops = pir_alloc_ops(mod, copy.operand_count);
                    nops[0] = copy.operands[0];
                    for (uint32_t k = 1u; k < copy.operand_count; ++k) {
                        PirOperand a = copy.operands[k];
                        bool orig_is_f32 = is_f32(a.type) ||
                                           (a.kind == PIR_OPER_VAL && a.u.val < orig_vals &&
                                            val_fp_kind[a.u.val] == 1u);
                        bool va_prom = is_var && (k - 1u) >= fixed_n && orig_is_f32 && !a.call_no_f32_promote;
                        PirOperand ma = map_fp_operand(a);
                        if (va_prom) {
                            PirOperand ext_arg = ma;
                            pir_op_strip_call_meta(&ext_arg);
                            PirValueId ext64 = emit_call1(mod, fi, b, &new_insts, "__extendsfdf2",
                                                          t32, t64, ext_arg, ln, lc, PIR_INVALID_ID, diag);
                            if (ext64 == PIR_INVALID_ID) {
                                PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); free(val_fp_kind);
                                return false;
                            }
                            PirOperand pe = pir_op_val(ext64, t64);
                            pe.has_explicit_type = true;
                            pe.call_stack_align  = ma.call_stack_align;
                            pe.call_force_stack  = ma.call_force_stack;
                            nops[k] = pe;
                        } else {
                            nops[k] = ma;
                        }
                    }
                    PirInst *slot = &mod->funcs.data[fi].insts.data[iid];
                    slot->ty       = map_fp_type(copy.ty);
                    slot->operands = nops;
                    PIR_VEC_PUSH(&new_insts, iid);
                    continue;
                }

                if (copy.op == PIR_OP_CONST && is_fp(copy.ty)) {
                    PirInst *slot = &mod->funcs.data[fi].insts.data[iid];
                    slot->ty = map_fp_type(copy.ty);
                    PirOperand *nops = pir_alloc_ops(mod, 1u);
                    nops[0] = map_fp_operand(copy.operands[0]);
                    slot->operands = nops;
                    PIR_VEC_PUSH(&new_insts, iid);
                    continue;
                }

                if (copy.op == PIR_OP_LOAD && is_fp(copy.ty)) {
                    mod->funcs.data[fi].insts.data[iid].ty = map_fp_type(copy.ty);
                    PIR_VEC_PUSH(&new_insts, iid);
                    continue;
                }

                if (copy.op == PIR_OP_STORE && is_fp(copy.ty)) {
                    PirInst *slot = &mod->funcs.data[fi].insts.data[iid];
                    slot->ty = map_fp_type(copy.ty);
                    PirOperand *nops = pir_alloc_ops(mod, 2u);
                    nops[0] = map_fp_operand(copy.operands[0]);
                    nops[1] = copy.operands[1];
                    slot->operands = nops;
                    PIR_VEC_PUSH(&new_insts, iid);
                    continue;
                }

                if (copy.op == PIR_OP_SELECT && is_fp(copy.ty)) {
                    PirInst *slot = &mod->funcs.data[fi].insts.data[iid];
                    slot->ty = map_fp_type(copy.ty);
                    PirOperand *nops = pir_alloc_ops(mod, 3u);
                    nops[0] = copy.operands[0];
                    nops[1] = map_fp_operand(copy.operands[1]);
                    nops[2] = map_fp_operand(copy.operands[2]);
                    slot->operands = nops;
                    PIR_VEC_PUSH(&new_insts, iid);
                    continue;
                }

                if (copy.op == PIR_OP_RET && copy.operand_count == 1u &&
                    (is_fp(copy.operands[0].type) ||
                     (copy.operands[0].kind == PIR_OPER_VAL && copy.operands[0].u.val < orig_vals &&
                      val_fp_kind[copy.operands[0].u.val] != 0u))) {
                    PirOperand *nops = pir_alloc_ops(mod, 1u);
                    nops[0] = map_fp_operand(copy.operands[0]);
                    mod->funcs.data[fi].insts.data[iid].operands = nops;
                    PIR_VEC_PUSH(&new_insts, iid);
                    continue;
                }

                if (copy.op == PIR_OP_BITCAST) {
                    bool dst_fp = is_fp(copy.ty);
                    bool src_fp = is_fp(copy.operands[0].type) ||
                                  (copy.operands[0].kind == PIR_OPER_VAL &&
                                   copy.operands[0].u.val < orig_vals &&
                                   val_fp_kind[copy.operands[0].u.val] != 0u);
                    if (dst_fp || src_fp) {
                        PirOperand mo = map_fp_operand(copy.operands[0]);
                        PirType dty = map_fp_type(copy.ty);
                        if (pir_type_eq(dty, mo.type)) {
                            if (mo.kind == PIR_OPER_VAL) {
                                SfAlias al = { copy.results[0], mo.u.val };
                                PIR_VEC_PUSH(&aliases, al);
                            } else {
                                emit_un(mod, &mod->funcs.data[fi], b, &new_insts, PIR_OP_CONST,
                                        dty, mo, ln, lc, copy.results[0]);
                            }
                        } else {
                            PirInst *slot = &mod->funcs.data[fi].insts.data[iid];
                            slot->ty = dty;
                            PirOperand *nops = pir_alloc_ops(mod, 1u);
                            nops[0] = mo;
                            slot->operands = nops;
                            PIR_VEC_PUSH(&new_insts, iid);
                        }
                        continue;
                    }
                    PIR_VEC_PUSH(&new_insts, iid);
                    continue;
                }

                if (copy.op == PIR_OP_FNEG) {
                    bool f32 = is_f32(copy.ty);
                    PirType ity = f32 ? t32 : t64;
                    uint64_t smask = f32 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
                    PirOperand a = map_fp_operand(copy.operands[0]);
                    emit_bin(mod, &mod->funcs.data[fi], b, &new_insts, PIR_OP_XOR, ity, 0u,
                             a, pir_op_imm_i(smask, false, ity), ln, lc, copy.results[0]);
                    continue;
                }

                if (copy.op == PIR_OP_FADD || copy.op == PIR_OP_FSUB ||
                    copy.op == PIR_OP_FMUL || copy.op == PIR_OP_FDIV) {
                    bool f32 = is_f32(copy.ty);
                    PirType ity = f32 ? t32 : t64;
                    const char *hname = (copy.op == PIR_OP_FADD) ? (f32 ? "__addsf3" : "__adddf3")
                                      : (copy.op == PIR_OP_FSUB) ? (f32 ? "__subsf3" : "__subdf3")
                                      : (copy.op == PIR_OP_FMUL) ? (f32 ? "__mulsf3" : "__muldf3")
                                                                 : (f32 ? "__divsf3" : "__divdf3");
                    PirOperand a = map_fp_operand(copy.operands[0]);
                    PirOperand c = map_fp_operand(copy.operands[1]);
                    if (emit_call2(mod, fi, b, &new_insts, hname, ity, ity, a, c, ln, lc,
                                   copy.results[0], diag) == PIR_INVALID_ID) {
                        PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); free(val_fp_kind);
                        return false;
                    }
                    continue;
                }

                if (copy.op == PIR_OP_FPEXT) {
                    PirOperand a = map_fp_operand(copy.operands[0]);
                    if (emit_call1(mod, fi, b, &new_insts, "__extendsfdf2", t32, t64, a, ln, lc,
                                   copy.results[0], diag) == PIR_INVALID_ID) {
                        PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); free(val_fp_kind);
                        return false;
                    }
                    continue;
                }

                if (copy.op == PIR_OP_FPTRUNC) {
                    PirOperand a = map_fp_operand(copy.operands[0]);
                    if (emit_call1(mod, fi, b, &new_insts, "__truncdfsf2", t64, t32, a, ln, lc,
                                   copy.results[0], diag) == PIR_INVALID_ID) {
                        PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); free(val_fp_kind);
                        return false;
                    }
                    continue;
                }

                if (copy.op == PIR_OP_SITOFP || copy.op == PIR_OP_UITOFP) {
                    bool is_s = (copy.op == PIR_OP_SITOFP);
                    bool d32  = is_f32(copy.ty);
                    PirType dty = d32 ? t32 : t64;
                    PirOperand src = copy.operands[0];
                    PirScalarType ssc = src.type.u.scalar;
                    if (ssc == PIR_SCALAR_I64) {
                        const char *hname = is_s ? (d32 ? "__floatdisf" : "__floatdidf")
                                                 : (d32 ? "__floatundisf" : "__floatundidf");
                        if (emit_call1(mod, fi, b, &new_insts, hname, t64, dty, src, ln, lc,
                                       copy.results[0], diag) == PIR_INVALID_ID) {
                            PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); free(val_fp_kind);
                            return false;
                        }
                    } else {
                        PirOperand s32 = src;
                        if (ssc == PIR_SCALAR_I8 || ssc == PIR_SCALAR_I16) {
                            PirValueId wv = emit_un(mod, &mod->funcs.data[fi], b, &new_insts,
                                                    is_s ? PIR_OP_SEXT : PIR_OP_ZEXT, t32, src,
                                                    ln, lc, PIR_INVALID_ID);
                            s32 = pir_op_val(wv, t32);
                        }
                        const char *hname = is_s ? (d32 ? "__floatsisf" : "__floatsidf")
                                                 : (d32 ? "__floatunsisf" : "__floatunsidf");
                        if (emit_call1(mod, fi, b, &new_insts, hname, t32, dty, s32, ln, lc,
                                       copy.results[0], diag) == PIR_INVALID_ID) {
                            PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); free(val_fp_kind);
                            return false;
                        }
                    }
                    continue;
                }

                if (copy.op >= PIR_OP_FCMP_OEQ && copy.op <= PIR_OP_FCMP_UGE) {
                    bool f32 = is_f32(copy.ty);
                    PirType ity = f32 ? t32 : t64;
                    PirOperand a = map_fp_operand(copy.operands[0]);
                    PirOperand c = map_fp_operand(copy.operands[1]);
                    PirValueId cmp = emit_call2(mod, fi, b, &new_insts,
                                                f32 ? "__pir_fcmp_sf" : "__pir_fcmp_df",
                                                ity, t8, a, c, ln, lc, PIR_INVALID_ID, diag);
                    if (cmp == PIR_INVALID_ID) {
                        PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); free(val_fp_kind);
                        return false;
                    }
                    PirFunction *fn = &mod->funcs.data[fi];
                    PirOperand cv = pir_op_val(cmp, t8);
                    PirOperand cm1 = pir_op_imm_i(0xFFu, false, t8); /* -1 */
                    PirOperand c0  = pir_op_imm_i(0u, false, t8);
                    PirOperand c1  = pir_op_imm_i(1u, false, t8);
                    PirOperand c2  = pir_op_imm_i(2u, false, t8);
                    PirValueId res = copy.results[0];
                    if (copy.op == PIR_OP_FCMP_OEQ) {
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t8, 0u, cv, c0, ln, lc, res);
                    } else if (copy.op == PIR_OP_FCMP_OLT) {
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t8, 0u, cv, cm1, ln, lc, res);
                    } else if (copy.op == PIR_OP_FCMP_OLE) {
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LE_S, t8, 0u, cv, c0, ln, lc, res);
                    } else if (copy.op == PIR_OP_FCMP_OGT) {
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t8, 0u, cv, c1, ln, lc, res);
                    } else if (copy.op == PIR_OP_FCMP_OGE) {
                        PirValueId t1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_GE_S, t8, 0u, cv, c0, ln, lc, PIR_INVALID_ID);
                        PirValueId t2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t8, 0u, cv, c2, ln, lc, PIR_INVALID_ID);
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(t1, t8), pir_op_val(t2, t8), ln, lc, res);
                    } else if (copy.op == PIR_OP_FCMP_ONE) {
                        PirValueId t1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t8, 0u, cv, c0, ln, lc, PIR_INVALID_ID);
                        PirValueId t2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t8, 0u, cv, c2, ln, lc, PIR_INVALID_ID);
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(t1, t8), pir_op_val(t2, t8), ln, lc, res);
                    } else if (copy.op == PIR_OP_FCMP_UEQ) {
                        PirValueId t1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t8, 0u, cv, c0, ln, lc, PIR_INVALID_ID);
                        PirValueId t2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t8, 0u, cv, c2, ln, lc, PIR_INVALID_ID);
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(t1, t8), pir_op_val(t2, t8), ln, lc, res);
                    } else if (copy.op == PIR_OP_FCMP_UNE) {
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t8, 0u, cv, c0, ln, lc, res);
                    } else if (copy.op == PIR_OP_FCMP_ULT) {
                        PirValueId t1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t8, 0u, cv, cm1, ln, lc, PIR_INVALID_ID);
                        PirValueId t2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t8, 0u, cv, c2, ln, lc, PIR_INVALID_ID);
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(t1, t8), pir_op_val(t2, t8), ln, lc, res);
                    } else if (copy.op == PIR_OP_FCMP_ULE) {
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t8, 0u, cv, c1, ln, lc, res);
                    } else if (copy.op == PIR_OP_FCMP_UGT) {
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_GE_S, t8, 0u, cv, c1, ln, lc, res);
                    } else { /* PIR_OP_FCMP_UGE */
                        emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t8, 0u, cv, cm1, ln, lc, res);
                    }
                    continue;
                }

                if (copy.op == PIR_OP_FPTOSI || copy.op == PIR_OP_FPTOUI) {
                    bool is_s   = (copy.op == PIR_OP_FPTOSI);
                    bool is_trp = (copy.flags & PIR_FLAG_TRAP) != 0u;
                    bool is_sat = (copy.flags & (PIR_FLAG_TRAP | PIR_FLAG_UNSAFE)) == 0u;
                    bool s32    = is_f32(copy.operands[0].type) ||
                                  (copy.operands[0].kind == PIR_OPER_VAL &&
                                   copy.operands[0].u.val < orig_vals &&
                                   val_fp_kind[copy.operands[0].u.val] == 1u);
                    PirType fity = s32 ? t32 : t64;
                    PirOperand src = map_fp_operand(copy.operands[0]);
                    PirScalarType dsc = copy.ty.u.scalar;
                    bool d64 = (dsc == PIR_SCALAR_I64);
                    PirType wty = d64 ? t64 : t32;

                    /* Exact IEEE-754 bounds for [min, max_excl). For [trap], float-to-int truncates
                     * toward zero before checking range, so values in (dmin - 1.0, dmin) are valid
                     * whenever dmin - 1.0 is strictly less than dmin in the source float format. */
                    double dmin = 0.0, dmax_excl = 0.0;
                    uint64_t imin_u = 0u, imax_u = 0u;
                    if (is_s) {
                        if (dsc == PIR_SCALAR_I8)       { dmin = -128.0; dmax_excl = 128.0; imin_u = (uint64_t)(int64_t)-128; imax_u = 127u; }
                        else if (dsc == PIR_SCALAR_I16) { dmin = -32768.0; dmax_excl = 32768.0; imin_u = (uint64_t)(int64_t)-32768; imax_u = 32767u; }
                        else if (dsc == PIR_SCALAR_I32) { dmin = -2147483648.0; dmax_excl = 2147483648.0; imin_u = (uint64_t)(int64_t)INT32_MIN; imax_u = (uint64_t)INT32_MAX; }
                        else                            { dmin = -9223372036854775808.0; dmax_excl = 9223372036854775808.0; imin_u = UINT64_C(0x8000000000000000); imax_u = UINT64_C(0x7FFFFFFFFFFFFFFF); }
                    } else {
                        dmin = 0.0; imin_u = 0u;
                        if (dsc == PIR_SCALAR_I8)       { dmax_excl = 256.0; imax_u = 255u; }
                        else if (dsc == PIR_SCALAR_I16) { dmax_excl = 65536.0; imax_u = 65535u; }
                        else if (dsc == PIR_SCALAR_I32) { dmax_excl = 4294967296.0; imax_u = UINT32_MAX; }
                        else                            { dmax_excl = 18446744073709551616.0; imax_u = UINT64_MAX; }
                    }
                    double dmin_m1 = dmin - 1.0;
                    bool trap_use_le = is_trp && (s32 ? ((float)dmin_m1 < (float)dmin)
                                                      : (dmin_m1 < dmin));
                    double dlo_cmp = trap_use_le ? dmin_m1 : dmin;
                    uint64_t bmin = 0u, bmax = 0u;
                    if (s32) {
                        float fmin = (float)dlo_cmp, fmax = (float)dmax_excl;
                        uint32_t umin = 0u, umax = 0u;
                        memcpy(&umin, &fmin, 4u); memcpy(&umax, &fmax, 4u);
                        bmin = (uint64_t)umin; bmax = (uint64_t)umax;
                    } else {
                        memcpy(&bmin, &dlo_cmp, 8u); memcpy(&bmax, &dmax_excl, 8u);
                    }

                    const char *cmp_fn = s32 ? "__pir_fcmp_sf" : "__pir_fcmp_df";
                    const char *fix_fn = d64
                        ? (is_s ? (s32 ? "__fixsfdi" : "__fixdfdi") : (s32 ? "__fixunssfdi" : "__fixunsdfdi"))
                        : (is_s ? (s32 ? "__fixsfsi" : "__fixdfsi") : (s32 ? "__fixunssfsi" : "__fixunsdfsi"));

                    PirValueId is_nan = PIR_INVALID_ID, too_lo = PIR_INVALID_ID, too_hi = PIR_INVALID_ID;
                    if (is_trp || is_sat) {
                        PirValueId cn = emit_call2(mod, fi, b, &new_insts, cmp_fn, fity, t8, src, src, ln, lc, PIR_INVALID_ID, diag);
                        PirValueId cl = emit_call2(mod, fi, b, &new_insts, cmp_fn, fity, t8, src, pir_op_imm_i(bmin, false, fity), ln, lc, PIR_INVALID_ID, diag);
                        PirValueId ch = emit_call2(mod, fi, b, &new_insts, cmp_fn, fity, t8, src, pir_op_imm_i(bmax, false, fity), ln, lc, PIR_INVALID_ID, diag);
                        if (cn == PIR_INVALID_ID || cl == PIR_INVALID_ID || ch == PIR_INVALID_ID) {
                            PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); free(val_fp_kind);
                            return false;
                        }
                        PirFunction *fn = &mod->funcs.data[fi];
                        is_nan = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t8, 0u,
                                          pir_op_val(cn, t8), pir_op_imm_i(2u, false, t8), ln, lc, PIR_INVALID_ID);
                        if (trap_use_le) {
                            too_lo = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LE_S, t8, 0u,
                                              pir_op_val(cl, t8), pir_op_imm_i(0u, false, t8), ln, lc, PIR_INVALID_ID);
                        } else {
                            too_lo = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t8, 0u,
                                              pir_op_val(cl, t8), pir_op_imm_i(0xFFu, false, t8), ln, lc, PIR_INVALID_ID);
                        }
                        PirValueId ch_ge0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_GE_S, t8, 0u,
                                                     pir_op_val(ch, t8), pir_op_imm_i(0u, false, t8), ln, lc, PIR_INVALID_ID);
                        PirValueId ch_ord = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t8, 0u,
                                                     pir_op_val(ch, t8), pir_op_imm_i(2u, false, t8), ln, lc, PIR_INVALID_ID);
                        too_hi = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u,
                                          pir_op_val(ch_ge0, t8), pir_op_val(ch_ord, t8), ln, lc, PIR_INVALID_ID);
                        if (is_trp) {
                            PirValueId b1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u,
                                                     pir_op_val(is_nan, t8), pir_op_val(too_lo, t8), ln, lc, PIR_INVALID_ID);
                            PirValueId bad = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u,
                                                      pir_op_val(b1, t8), pir_op_val(too_hi, t8), ln, lc, PIR_INVALID_ID);
                            emit_trap_if_sf(mod, fn, b, &new_insts, bad, ln, lc);
                        }
                    }

                    bool need_trunc = (dsc == PIR_SCALAR_I8 || dsc == PIR_SCALAR_I16);
                    PirValueId raw_dst = (!is_sat && !need_trunc) ? copy.results[0] : PIR_INVALID_ID;
                    PirValueId raw = emit_call1(mod, fi, b, &new_insts, fix_fn, fity, wty, src,
                                                ln, lc, raw_dst, diag);
                    if (raw == PIR_INVALID_ID) {
                        PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); free(val_fp_kind);
                        return false;
                    }
                    PirFunction *fn = &mod->funcs.data[fi];
                    PirValueId cur = raw;
                    if (is_sat) {
                        uint64_t mask = d64 ? UINT64_MAX : UINT32_MAX;
                        PirValueId s1 = emit_sel(mod, fn, b, &new_insts, wty, pir_op_val(too_lo, t8),
                                                 pir_op_imm_i(imin_u & mask, false, wty),
                                                 pir_op_val(cur, wty), ln, lc, PIR_INVALID_ID);
                        PirValueId s2 = emit_sel(mod, fn, b, &new_insts, wty, pir_op_val(too_hi, t8),
                                                 pir_op_imm_i(imax_u & mask, false, wty),
                                                 pir_op_val(s1, wty), ln, lc, PIR_INVALID_ID);
                        cur = emit_sel(mod, fn, b, &new_insts, wty, pir_op_val(is_nan, t8),
                                       pir_op_imm_i(0u, false, wty),
                                       pir_op_val(s2, wty), ln, lc,
                                       need_trunc ? PIR_INVALID_ID : copy.results[0]);
                    }
                    if (need_trunc) {
                        emit_un(mod, fn, b, &new_insts, PIR_OP_TRUNC, copy.ty,
                                pir_op_val(cur, wty), ln, lc, copy.results[0]);
                    }
                    continue;
                }

                PIR_VEC_PUSH(&new_insts, iid);
            }
            PIR_VEC_FREE(&mod->funcs.data[fi].blocks.data[b].insts);
            mod->funcs.data[fi].blocks.data[b].insts = new_insts;
        }

        PirFunction *fn = &mod->funcs.data[fi];
        for (uint32_t i = 0u; i < aliases.len; ++i) {
            PirValueId root = aliases.data[i].to;
            for (uint32_t s = 0u; s <= aliases.len; ++s) {
                bool moved = false;
                for (uint32_t k = 0u; k < aliases.len; ++k) {
                    if (aliases.data[k].from == root && aliases.data[k].to != aliases.data[i].from) {
                        root = aliases.data[k].to;
                        moved = true;
                        break;
                    }
                }
                if (!moved) break;
            }
            if (root != aliases.data[i].from) {
                pir_fn_replace_val_uses(fn, aliases.data[i].from, root);
            }
        }
        PIR_VEC_FREE(&aliases);
        free(val_fp_kind);
    }
    return true;
}
