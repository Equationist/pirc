/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lower/lower.h"

#include <stdlib.h>
#include <string.h>

/* Split descriptor: every oversized vector ValueId expands into `count` pieces
 * (count = 2 for v256 -> 2*v128 when max_simd_bits==128; count = lanes for scalarize). */
typedef struct {
    PirType     piece_ty;
    uint32_t    count;       /* 0 = not split */
    PirValueId *ids;         /* Arena-allocated array of length `count` */
} VecSplit;

typedef struct {
    PirValueId from;
    PirValueId to;
} SimdAlias;

static bool needs_split(PirType t, uint16_t max_bits) {
    return t.kind == PIR_TYPE_VECTOR && pir_vec_bits(t.u.vec) > max_bits;
}

static PirVecType half_vec_type(PirVecType v) {
    switch (v) {
        case PIR_VEC_V32I8:  return PIR_VEC_V16I8;
        case PIR_VEC_V16I16: return PIR_VEC_V8I16;
        case PIR_VEC_V8I32:  return PIR_VEC_V4I32;
        case PIR_VEC_V4I64:  return PIR_VEC_V2I64;
        case PIR_VEC_V8F32:  return PIR_VEC_V4F32;
        case PIR_VEC_V4F64:  return PIR_VEC_V2F64;
        case PIR_VEC_NONE:
        case PIR_VEC_V8I8:  case PIR_VEC_V4I16: case PIR_VEC_V2I32: case PIR_VEC_V2F32:
        case PIR_VEC_V16I8: case PIR_VEC_V8I16: case PIR_VEC_V4I32: case PIR_VEC_V2I64:
        case PIR_VEC_V4F32: case PIR_VEC_V2F64:
            return PIR_VEC_NONE;
    }
    return PIR_VEC_NONE;
}

static void split_spec(PirVecType v, uint16_t max_bits, PirType *out_piece_ty, uint32_t *out_count) {
    if (max_bits == 0u) {
        *out_piece_ty = pir_type_scalar(pir_vec_elem_scalar(v));
        *out_count    = pir_vec_lanes(v);
    } else {
        /* 256-bit -> 2 x 128-bit */
        *out_piece_ty = pir_type_vec(half_vec_type(v));
        *out_count    = 2u;
    }
}

static uint32_t aligned_offset_align(uint32_t base_align, uint64_t off, uint32_t max_al) {
    uint32_t al = base_align < max_al ? base_align : max_al;
    while (al > 1u && (off % al) != 0u) al >>= 1u;
    return al > 0u ? al : 1u;
}

static PirValueId emit_padd(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                            PirOperand base, uint64_t off, PirType idx_ty,
                            uint32_t ln, uint32_t lc) {
    if (off == 0u && base.kind == PIR_OPER_VAL) return base.u.val;
    PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
    pa.flags = PIR_FLAG_INBOUNDS;
    pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
    pa.operands[0] = base; pa.operands[1] = pir_op_imm_i(off, false, idx_ty);
    pa.results[0] = pir_fn_new_val(mod, fn, "vp", pir_type_scalar(PIR_SCALAR_PTR), blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, pa);
    return pa.results[0];
}

static void expand_branch_target_simd(PirModule *mod, PirBranchTarget *bt,
                                      const VecSplit *splits, uint32_t orig_vals) {
    if (bt->arg_count == 0u) return;
    bool has_any = false;
    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        if (bt->args[k].kind == PIR_OPER_VAL && bt->args[k].u.val < orig_vals &&
            splits[bt->args[k].u.val].count > 0u) {
            has_any = true;
            break;
        }
    }
    if (!has_any) return;
    PIR_VEC(PirOperand) nargs = {0};
    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        PirOperand a = bt->args[k];
        if (a.kind == PIR_OPER_VAL && a.u.val < orig_vals && splits[a.u.val].count > 0u) {
            const VecSplit *sp = &splits[a.u.val];
            for (uint32_t p = 0u; p < sp->count; ++p) {
                PIR_VEC_PUSH(&nargs, pir_op_val(sp->ids[p], sp->piece_ty));
            }
        } else {
            PIR_VEC_PUSH(&nargs, a);
        }
    }
    bt->arg_count = nargs.len;
    bt->args      = pir_alloc_ops(mod, nargs.len);
    memcpy(bt->args, nargs.data, nargs.len * sizeof(PirOperand));
    PIR_VEC_FREE(&nargs);
}

static bool is_cmp_op(PirOpcode op) {
    return (op >= PIR_OP_CMP_EQ && op <= PIR_OP_CMP_GE_U) ||
           (op >= PIR_OP_FCMP_OEQ && op <= PIR_OP_FCMP_UGE);
}

static bool is_vec_cast_op(PirOpcode op) {
    return op == PIR_OP_TRUNC  || op == PIR_OP_SEXT   || op == PIR_OP_ZEXT   ||
           op == PIR_OP_FPTRUNC || op == PIR_OP_FPEXT ||
           op == PIR_OP_SITOFP || op == PIR_OP_UITOFP ||
           op == PIR_OP_FPTOSI || op == PIR_OP_FPTOUI;
}

static void set_simd_sret_cabi(const PirTargetSpec *target, PirCallingConv cc,
                               PirCabiRetInfo *cabi_ret) {
    if (cc != PIR_CC_C) return;
    memset(cabi_ret, 0, sizeof(*cabi_ret));
    if (target->arch == PIR_ARCH_ARM64) {
        cabi_ret->kind = PIR_CABI_RET_SRET_X8;
    } else if (target->arch == PIR_ARCH_X86_64) {
        cabi_ret->kind = PIR_CABI_RET_SRET_RAX;
    }
}

static bool legalize_simd_func(PirModule *mod, const PirTargetSpec *target, PirFuncId fi, PirDiag *diag) {
    PirFunction *fn = &mod->funcs.data[fi];
    if (!fn->is_defined) return true;
    uint16_t max_bits = target->caps.max_simd_bits;

    bool ret_is_ov = needs_split(fn->ret_type, max_bits);
    bool has_oversized = ret_is_ov;
    for (uint32_t v = 0u; v < fn->values.len && !has_oversized; ++v) {
        if (needs_split(fn->values.data[v].type, max_bits)) { has_oversized = true; }
    }
    for (uint32_t i = 0u; i < fn->insts.len && !has_oversized; ++i) {
        if (needs_split(fn->insts.data[i].ty, max_bits)) { has_oversized = true; }
    }
    if (!has_oversized) return true;

    PirType t8     = pir_type_scalar(PIR_SCALAR_I8);
    PirType tptr   = pir_type_scalar(PIR_SCALAR_PTR);
    PirType tvoid  = pir_type_scalar(PIR_SCALAR_VOID);
    PirType idx_ty = pir_type_scalar(target->ptr_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);
    PirValueId ret_out_val = PIR_INVALID_ID;

    uint32_t orig_vals = fn->values.len;
    VecSplit *splits = (VecSplit *)calloc(orig_vals + 1u, sizeof(VecSplit));
    if (!splits) abort();

    for (uint32_t v = 0u; v < orig_vals; ++v) {
        PirType vt = fn->values.data[v].type;
        if (!needs_split(vt, max_bits)) continue;
        PirType pty;
        uint32_t cnt = 0u;
        split_spec(vt.u.vec, max_bits, &pty, &cnt);
        splits[v].piece_ty = pty;
        splits[v].count    = cnt;
        splits[v].ids      = PIR_ARENA_NEW_ARRAY(&mod->arena, PirValueId, cnt);
        PirBlockId db = fn->values.data[v].def_block;
        uint32_t   pi = fn->values.data[v].param_index;
        fn->values.data[v].type = pty;
        splits[v].ids[0] = v;
        for (uint32_t p = 1u; p < cnt; ++p) {
            splits[v].ids[p] = pir_fn_new_val(mod, fn, "vp", pty, db, PIR_INVALID_ID, pi);
        }
    }

    /* Expand block params. */
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        PirValueIdVec nparams = {0};
        for (uint32_t k = 0u; k < fn->blocks.data[b].params.len; ++k) {
            PirValueId pv = fn->blocks.data[b].params.data[k];
            if (pv < orig_vals && splits[pv].count > 0u) {
                for (uint32_t p = 0u; p < splits[pv].count; ++p) {
                    PIR_VEC_PUSH(&nparams, splits[pv].ids[p]);
                }
            } else {
                PIR_VEC_PUSH(&nparams, pv);
            }
        }
        for (uint32_t k = 0u; k < nparams.len; ++k) {
            fn->values.data[nparams.data[k]].param_index = k;
        }
        PIR_VEC_FREE(&fn->blocks.data[b].params);
        fn->blocks.data[b].params = nparams;
    }

    /* Expand function params if any are oversized vectors or return is an oversized vector. */
    bool has_split_fp = ret_is_ov;
    for (uint32_t i = 0u; i < fn->param_count && !has_split_fp; ++i) {
        if (needs_split(fn->params[i].type, max_bits)) { has_split_fp = true; }
    }
    if (has_split_fp) {
        PIR_VEC(PirParamDecl) nfp = {0};
        if (ret_is_ov) {
            ret_out_val = pir_fn_new_val(mod, fn, "simd_out", tptr, PIR_INVALID_ID, PIR_INVALID_ID, 0u);
            PirParamDecl op;
            memset(&op, 0, sizeof(op));
            op.name     = fn->values.data[ret_out_val].name;
            op.type     = tptr;
            op.abi_type = tvoid;
            op.val_id   = ret_out_val;
            PIR_VEC_PUSH(&nfp, op);

            fn->ret_type = tvoid;
            set_simd_sret_cabi(target, fn->cc, &fn->cabi_ret);
        }
        for (uint32_t i = 0u; i < fn->param_count; ++i) {
            if (needs_split(fn->params[i].type, max_bits)) {
                const VecSplit *sp = &splits[fn->params[i].val_id];
                for (uint32_t p = 0u; p < sp->count; ++p) {
                    PirParamDecl pd = fn->params[i];
                    pd.type   = sp->piece_ty;
                    pd.val_id = sp->ids[p];
                    pd.name   = fn->values.data[pd.val_id].name;
                    if (p > 0u) pd.abi_stack_align = 0u;
                    PIR_VEC_PUSH(&nfp, pd);
                }
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

    PIR_VEC(SimdAlias) aliases = {0};
    PirInstIdVec entry_allocas = {0};

    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        PirInstIdVec new_insts = {0};
        uint32_t old_len = fn->blocks.data[b].insts.len;

        for (uint32_t j = 0u; j < old_len; ++j) {
            PirInstId iid = fn->blocks.data[b].insts.data[j];
            PirInst copy  = fn->insts.data[iid];
            uint32_t ln = copy.src_line, lc = copy.loc_idx;

            if (copy.op == PIR_OP_JMP || copy.op == PIR_OP_BR || copy.op == PIR_OP_SWITCH) {
                PirInst *slot = &fn->insts.data[iid];
                expand_branch_target_simd(mod, &slot->targets[0], splits, orig_vals);
                if (copy.op == PIR_OP_BR) {
                    expand_branch_target_simd(mod, &slot->targets[1], splits, orig_vals);
                } else if (copy.op == PIR_OP_SWITCH) {
                    for (uint32_t c = 0u; c < slot->switch_case_count; ++c) {
                        expand_branch_target_simd(mod, &slot->switch_cases[c].target, splits, orig_vals);
                    }
                }
                PIR_VEC_PUSH(&new_insts, iid);
                continue;
            }

            /* Check if any operand or primary type needs splitting. */
            bool op_split = needs_split(copy.ty, max_bits);
            for (uint32_t k = 0u; k < copy.operand_count && !op_split; ++k) {
                if (copy.operands[k].kind == PIR_OPER_VAL &&
                    copy.operands[k].u.val < orig_vals &&
                    splits[copy.operands[k].u.val].count > 0u) {
                    op_split = true;
                }
            }
            if (!op_split) {
                PIR_VEC_PUSH(&new_insts, iid);
                continue;
            }

            PirValueId r0 = copy.results[0];
            const VecSplit *rsp = (r0 != PIR_INVALID_ID && r0 < orig_vals && splits[r0].count > 0u)
                                  ? &splits[r0] : NULL;

            if (copy.op == PIR_OP_SPLAT) {
                if (max_bits == 0u) {
                    /* Scalarized splat: every lane is the scalar operand! */
                    PirOperand sc = copy.operands[0];
                    for (uint32_t p = 0u; p < rsp->count; ++p) {
                        if (sc.kind == PIR_OPER_VAL) {
                            SimdAlias al = { rsp->ids[p], sc.u.val };
                            PIR_VEC_PUSH(&aliases, al);
                        } else {
                            PirInst c = pir_make_inst(PIR_OP_CONST, rsp->piece_ty, ln, lc);
                            c.operand_count = 1u; c.operands = pir_alloc_ops(mod, 1u);
                            c.operands[0] = sc; c.results[0] = rsp->ids[p];
                            pir_fn_append_inst(fn, &new_insts, c);
                        }
                    }
                } else {
                    for (uint32_t p = 0u; p < rsp->count; ++p) {
                        PirInst sp = pir_make_inst(PIR_OP_SPLAT, rsp->piece_ty, ln, lc);
                        sp.operand_count = 1u; sp.operands = pir_alloc_ops(mod, 1u);
                        sp.operands[0] = copy.operands[0];
                        sp.results[0]  = rsp->ids[p];
                        pir_fn_append_inst(fn, &new_insts, sp);
                    }
                }
            } else if (copy.op == PIR_OP_EXTRACT) {
                const VecSplit *vsp = &splits[copy.operands[0].u.val];
                uint64_t idx = copy.imm_u64;
                if (max_bits == 0u) {
                    SimdAlias al = { r0, vsp->ids[idx] };
                    PIR_VEC_PUSH(&aliases, al);
                } else {
                    uint32_t half_lanes = pir_vec_lanes(vsp->piece_ty.u.vec);
                    uint32_t which = (idx < half_lanes) ? 0u : 1u;
                    uint64_t sub   = (idx < half_lanes) ? idx : (idx - half_lanes);
                    PirInst ex = pir_make_inst(PIR_OP_EXTRACT, vsp->piece_ty, ln, lc);
                    ex.imm_u64 = sub;
                    ex.operand_count = 1u; ex.operands = pir_alloc_ops(mod, 1u);
                    ex.operands[0] = pir_op_val(vsp->ids[which], vsp->piece_ty);
                    ex.results[0]  = r0;
                    pir_fn_append_inst(fn, &new_insts, ex);
                }
            } else if (copy.op == PIR_OP_INSERT) {
                const VecSplit *vsp = &splits[copy.operands[0].u.val];
                uint64_t idx = copy.imm_u64;
                if (max_bits == 0u) {
                    for (uint32_t p = 0u; p < rsp->count; ++p) {
                        if (p == idx) {
                            if (copy.operands[1].kind == PIR_OPER_VAL) {
                                SimdAlias al = { rsp->ids[p], copy.operands[1].u.val };
                                PIR_VEC_PUSH(&aliases, al);
                            } else {
                                PirInst c = pir_make_inst(PIR_OP_CONST, rsp->piece_ty, ln, lc);
                                c.operand_count = 1u; c.operands = pir_alloc_ops(mod, 1u);
                                c.operands[0] = copy.operands[1]; c.results[0] = rsp->ids[p];
                                pir_fn_append_inst(fn, &new_insts, c);
                            }
                        } else {
                            SimdAlias al = { rsp->ids[p], vsp->ids[p] };
                            PIR_VEC_PUSH(&aliases, al);
                        }
                    }
                } else {
                    uint32_t half_lanes = pir_vec_lanes(vsp->piece_ty.u.vec);
                    uint32_t which = (idx < half_lanes) ? 0u : 1u;
                    uint64_t sub   = (idx < half_lanes) ? idx : (idx - half_lanes);
                    for (uint32_t p = 0u; p < 2u; ++p) {
                        if (p == which) {
                            PirInst ins = pir_make_inst(PIR_OP_INSERT, rsp->piece_ty, ln, lc);
                            ins.imm_u64 = sub;
                            ins.operand_count = 2u; ins.operands = pir_alloc_ops(mod, 2u);
                            ins.operands[0] = pir_op_val(vsp->ids[p], rsp->piece_ty);
                            ins.operands[1] = copy.operands[1];
                            ins.results[0]  = rsp->ids[p];
                            pir_fn_append_inst(fn, &new_insts, ins);
                        } else {
                            SimdAlias al = { rsp->ids[p], vsp->ids[p] };
                            PIR_VEC_PUSH(&aliases, al);
                        }
                    }
                }
            } else if (copy.op == PIR_OP_SHUFFLE) {
                const VecSplit *a = &splits[copy.operands[0].u.val];
                const VecSplit *bv = &splits[copy.operands[1].u.val];
                uint32_t lanes = pir_vec_lanes(copy.ty.u.vec);
                if (max_bits == 0u) {
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        uint8_t idx = copy.shuffle_mask[l];
                        PirValueId src_v = (idx < lanes) ? a->ids[idx] : bv->ids[idx - lanes];
                        SimdAlias al = { rsp->ids[l], src_v };
                        PIR_VEC_PUSH(&aliases, al);
                    }
                } else {
                    /* Build each 128-bit output half via extract + insert (handles arbitrary cross-half). */
                    uint32_t h = lanes / 2u;
                    PirType elem_ty = pir_type_scalar(pir_vec_elem_scalar(copy.ty.u.vec));
                    PirValueId sources[4] = { a->ids[0], a->ids[1], bv->ids[0], bv->ids[1] };
                    for (uint32_t half = 0u; half < 2u; ++half) {
                        PirValueId cur_v = PIR_INVALID_ID;
                        for (uint32_t l = 0u; l < h; ++l) {
                            uint8_t idx = copy.shuffle_mask[half * h + l];
                            uint32_t src_half = idx / h;
                            uint32_t src_lane = idx % h;
                            PirInst ex = pir_make_inst(PIR_OP_EXTRACT, rsp->piece_ty, ln, lc);
                            ex.imm_u64 = src_lane;
                            ex.operand_count = 1u; ex.operands = pir_alloc_ops(mod, 1u);
                            ex.operands[0] = pir_op_val(sources[src_half], rsp->piece_ty);
                            ex.results[0]  = pir_fn_new_val(mod, fn, "sh_e", elem_ty, b, PIR_INVALID_ID, 0u);
                            pir_fn_append_inst(fn, &new_insts, ex);

                            if (l == 0u) {
                                PirInst sp = pir_make_inst(PIR_OP_SPLAT, rsp->piece_ty, ln, lc);
                                sp.operand_count = 1u; sp.operands = pir_alloc_ops(mod, 1u);
                                sp.operands[0] = pir_op_val(ex.results[0], elem_ty);
                                sp.results[0]  = (h == 1u) ? rsp->ids[half]
                                                           : pir_fn_new_val(mod, fn, "sh_v", rsp->piece_ty, b, PIR_INVALID_ID, 0u);
                                pir_fn_append_inst(fn, &new_insts, sp);
                                cur_v = sp.results[0];
                            } else {
                                PirInst ins = pir_make_inst(PIR_OP_INSERT, rsp->piece_ty, ln, lc);
                                ins.imm_u64 = l;
                                ins.operand_count = 2u; ins.operands = pir_alloc_ops(mod, 2u);
                                ins.operands[0] = pir_op_val(cur_v, rsp->piece_ty);
                                ins.operands[1] = pir_op_val(ex.results[0], elem_ty);
                                ins.results[0]  = (l + 1u == h) ? rsp->ids[half]
                                                                : pir_fn_new_val(mod, fn, "sh_v", rsp->piece_ty, b, PIR_INVALID_ID, 0u);
                                pir_fn_append_inst(fn, &new_insts, ins);
                                cur_v = ins.results[0];
                            }
                        }
                    }
                }
            } else if (copy.op == PIR_OP_LOAD) {
                uint32_t piece_bytes = (uint32_t)(pir_type_bits(rsp->piece_ty, target->ptr_bytes) / 8u);
                for (uint32_t p = 0u; p < rsp->count; ++p) {
                    uint64_t off = (uint64_t)p * piece_bytes;
                    PirValueId ptr_p = emit_padd(mod, fn, b, &new_insts, copy.operands[0], off, idx_ty, ln, lc);
                    PirInst ld = pir_make_inst(PIR_OP_LOAD, rsp->piece_ty, ln, lc);
                    ld.align = aligned_offset_align(copy.align, off, piece_bytes);
                    ld.flags = copy.flags;
                    ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
                    ld.operands[0] = pir_op_val(ptr_p, tptr);
                    ld.results[0]  = rsp->ids[p];
                    pir_fn_append_inst(fn, &new_insts, ld);
                }
            } else if (copy.op == PIR_OP_STORE) {
                const VecSplit *vsp = &splits[copy.operands[0].u.val];
                uint32_t piece_bytes = (uint32_t)(pir_type_bits(vsp->piece_ty, target->ptr_bytes) / 8u);
                for (uint32_t p = 0u; p < vsp->count; ++p) {
                    uint64_t off = (uint64_t)p * piece_bytes;
                    PirValueId ptr_p = emit_padd(mod, fn, b, &new_insts, copy.operands[1], off, idx_ty, ln, lc);
                    PirInst st = pir_make_inst(PIR_OP_STORE, vsp->piece_ty, ln, lc);
                    st.align = aligned_offset_align(copy.align, off, piece_bytes);
                    st.flags = copy.flags;
                    st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
                    st.operands[0] = pir_op_val(vsp->ids[p], vsp->piece_ty);
                    st.operands[1] = pir_op_val(ptr_p, tptr);
                    pir_fn_append_inst(fn, &new_insts, st);
                }
            } else if (copy.op == PIR_OP_BITCAST) {
                PirValueId src_v = (copy.operands[0].kind == PIR_OPER_VAL)
                                   ? copy.operands[0].u.val : PIR_INVALID_ID;
                const VecSplit *ssp = (src_v < orig_vals && splits[src_v].count > 0u) ? &splits[src_v] : NULL;
                if (ssp && rsp && ssp->count == rsp->count &&
                    pir_type_bits(ssp->piece_ty, target->ptr_bytes) ==
                    pir_type_bits(rsp->piece_ty, target->ptr_bytes)) {
                    for (uint32_t p = 0u; p < rsp->count; ++p) {
                        PirInst bc = pir_make_inst(PIR_OP_BITCAST, rsp->piece_ty, ln, lc);
                        bc.operand_count = 1u; bc.operands = pir_alloc_ops(mod, 1u);
                        bc.operands[0] = pir_op_val(ssp->ids[p], ssp->piece_ty);
                        bc.results[0]  = rsp->ids[p];
                        pir_fn_append_inst(fn, &new_insts, bc);
                    }
                } else {
                    /* General cross-lane-count bitcast via entry-block stack slot. */
                    uint32_t total_bytes = (uint32_t)(pir_type_bits(copy.ty, target->ptr_bytes) / 8u);
                    PirValueId vbc_v = pir_fn_emit_entry_alloca(mod, fn, "vbc",
                                                                total_bytes, total_bytes,
                                                                ln, lc, &entry_allocas);
                    if (ssp) {
                        uint32_t pb = (uint32_t)(pir_type_bits(ssp->piece_ty, target->ptr_bytes) / 8u);
                        for (uint32_t p = 0u; p < ssp->count; ++p) {
                            PirValueId sp = emit_padd(mod, fn, b, &new_insts, pir_op_val(vbc_v, tptr), (uint64_t)p * pb, idx_ty, ln, lc);
                            PirInst st = pir_make_inst(PIR_OP_STORE, ssp->piece_ty, ln, lc);
                            st.align = pb; st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
                            st.operands[0] = pir_op_val(ssp->ids[p], ssp->piece_ty);
                            st.operands[1] = pir_op_val(sp, tptr);
                            pir_fn_append_inst(fn, &new_insts, st);
                        }
                    } else {
                        PirType st_ty = copy.operands[0].type;
                        PirOperand st_op = copy.operands[0];
                        if (st_op.kind == PIR_OPER_IMM_INT &&
                            !(st_ty.kind == PIR_TYPE_SCALAR && pir_scalar_is_int(st_ty.u.scalar))) {
                            st_ty = pir_type_scalar(total_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);
                            st_op.type = st_ty;
                        }
                        PirInst st = pir_make_inst(PIR_OP_STORE, st_ty, ln, lc);
                        st.align = total_bytes; st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
                        st.operands[0] = st_op;
                        st.operands[1] = pir_op_val(vbc_v, tptr);
                        pir_fn_append_inst(fn, &new_insts, st);
                    }
                    if (rsp) {
                        uint32_t pb = (uint32_t)(pir_type_bits(rsp->piece_ty, target->ptr_bytes) / 8u);
                        for (uint32_t p = 0u; p < rsp->count; ++p) {
                            PirValueId lp = emit_padd(mod, fn, b, &new_insts, pir_op_val(vbc_v, tptr), (uint64_t)p * pb, idx_ty, ln, lc);
                            PirInst ld = pir_make_inst(PIR_OP_LOAD, rsp->piece_ty, ln, lc);
                            ld.align = pb; ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
                            ld.operands[0] = pir_op_val(lp, tptr);
                            ld.results[0]  = rsp->ids[p];
                            pir_fn_append_inst(fn, &new_insts, ld);
                        }
                    } else {
                        PirInst ld = pir_make_inst(PIR_OP_LOAD, copy.ty, ln, lc);
                        ld.align = total_bytes; ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
                        ld.operands[0] = pir_op_val(vbc_v, tptr);
                        ld.results[0]  = r0;
                        pir_fn_append_inst(fn, &new_insts, ld);
                    }
                }
            } else if (copy.op == PIR_OP_SELECT) {
                const VecSplit *csp = &splits[copy.operands[0].u.val];
                const VecSplit *tsp = &splits[copy.operands[1].u.val];
                const VecSplit *fsp = &splits[copy.operands[2].u.val];
                for (uint32_t p = 0u; p < rsp->count; ++p) {
                    PirOperand cond_p = pir_op_val(csp->ids[p], csp->piece_ty);
                    if (max_bits == 0u) {
                        /* Convert -1/0 integer mask lane to i8 1/0 for scalar select. */
                        PirInst ne = pir_make_inst(PIR_OP_CMP_NE, csp->piece_ty, ln, lc);
                        ne.operand_count = 2u; ne.operands = pir_alloc_ops(mod, 2u);
                        ne.operands[0] = cond_p;
                        ne.operands[1] = pir_op_imm_i(0u, false, csp->piece_ty);
                        ne.results[0]  = pir_fn_new_val(mod, fn, "sb", t8, b, PIR_INVALID_ID, 0u);
                        pir_fn_append_inst(fn, &new_insts, ne);
                        cond_p = pir_op_val(ne.results[0], t8);
                    }
                    PirInst sel = pir_make_inst(PIR_OP_SELECT, rsp->piece_ty, ln, lc);
                    sel.operand_count = 3u; sel.operands = pir_alloc_ops(mod, 3u);
                    sel.operands[0] = cond_p;
                    sel.operands[1] = pir_op_val(tsp->ids[p], rsp->piece_ty);
                    sel.operands[2] = pir_op_val(fsp->ids[p], rsp->piece_ty);
                    sel.results[0]  = rsp->ids[p];
                    pir_fn_append_inst(fn, &new_insts, sel);
                }
            } else if (is_cmp_op(copy.op)) {
                const VecSplit *asp = &splits[copy.operands[0].u.val];
                const VecSplit *bsp = &splits[copy.operands[1].u.val];
                for (uint32_t p = 0u; p < rsp->count; ++p) {
                    if (max_bits == 0u) {
                        /* Scalar cmp returns i8 (0/1); widen to -1/0 mask lane. */
                        PirInst c = pir_make_inst(copy.op, asp->piece_ty, ln, lc);
                        c.operand_count = 2u; c.operands = pir_alloc_ops(mod, 2u);
                        c.operands[0] = pir_op_val(asp->ids[p], asp->piece_ty);
                        c.operands[1] = pir_op_val(bsp->ids[p], bsp->piece_ty);
                        c.results[0]  = pir_fn_new_val(mod, fn, "vc", t8, b, PIR_INVALID_ID, 0u);
                        pir_fn_append_inst(fn, &new_insts, c);

                        PirInst m = pir_make_inst(PIR_OP_SELECT, rsp->piece_ty, ln, lc);
                        m.operand_count = 3u; m.operands = pir_alloc_ops(mod, 3u);
                        m.operands[0] = pir_op_val(c.results[0], t8);
                        m.operands[1] = pir_op_imm_i(1u, true, rsp->piece_ty);
                        m.operands[2] = pir_op_imm_i(0u, false, rsp->piece_ty);
                        m.results[0]  = rsp->ids[p];
                        pir_fn_append_inst(fn, &new_insts, m);
                    } else {
                        PirInst c = pir_make_inst(copy.op, asp->piece_ty, ln, lc);
                        c.operand_count = 2u; c.operands = pir_alloc_ops(mod, 2u);
                        c.operands[0] = pir_op_val(asp->ids[p], asp->piece_ty);
                        c.operands[1] = pir_op_val(bsp->ids[p], bsp->piece_ty);
                        c.results[0]  = rsp->ids[p];
                        pir_fn_append_inst(fn, &new_insts, c);
                    }
                }
            } else if (is_vec_cast_op(copy.op) && copy.ty.kind == PIR_TYPE_VECTOR &&
                       copy.operand_count == 1u && copy.operands[0].kind == PIR_OPER_VAL &&
                       copy.operands[0].type.kind == PIR_TYPE_VECTOR) {
                PirValueId src_v = copy.operands[0].u.val;
                const VecSplit *ssp = (src_v < orig_vals && splits[src_v].count > 0u) ? &splits[src_v] : NULL;
                if (ssp && rsp && ssp->count == rsp->count) {
                    for (uint32_t p = 0u; p < rsp->count; ++p) {
                        PirInst ni = pir_make_inst(copy.op, rsp->piece_ty, ln, lc);
                        ni.flags         = copy.flags;
                        ni.operand_count = 1u;
                        ni.operands      = pir_alloc_ops(mod, 1u);
                        ni.operands[0]   = pir_op_val(ssp->ids[p], ssp->piece_ty);
                        ni.results[0]    = rsp->ids[p];
                        pir_fn_append_inst(fn, &new_insts, ni);
                    }
                } else {
                    /* Cross-width vector cast where one side is split (2x128b) and the other is <=128b. */
                    PirVecType svt = copy.operands[0].type.u.vec;
                    PirVecType dvt = copy.ty.u.vec;
                    uint32_t lanes = pir_vec_lanes(dvt);
                    PirType sel_ty = pir_type_scalar(pir_vec_elem_scalar(svt));
                    PirType del_ty = pir_type_scalar(pir_vec_elem_scalar(dvt));
                    uint32_t half_lanes = lanes / 2u;

                    PirValueId dst_cur[2] = { PIR_INVALID_ID, PIR_INVALID_ID };
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        PirValueId src_lane_v = PIR_INVALID_ID;
                        if (ssp == NULL) {
                            PirInst ex = pir_make_inst(PIR_OP_EXTRACT, copy.operands[0].type, ln, lc);
                            ex.imm_u64 = l;
                            ex.operand_count = 1u; ex.operands = pir_alloc_ops(mod, 1u);
                            ex.operands[0] = copy.operands[0];
                            ex.results[0]  = pir_fn_new_val(mod, fn, "vc_e", sel_ty, b, PIR_INVALID_ID, 0u);
                            pir_fn_append_inst(fn, &new_insts, ex);
                            src_lane_v = ex.results[0];
                        } else if (max_bits == 0u) {
                            src_lane_v = ssp->ids[l];
                        } else {
                            uint32_t sh = l / half_lanes;
                            uint32_t sl = l % half_lanes;
                            PirInst ex = pir_make_inst(PIR_OP_EXTRACT, ssp->piece_ty, ln, lc);
                            ex.imm_u64 = sl;
                            ex.operand_count = 1u; ex.operands = pir_alloc_ops(mod, 1u);
                            ex.operands[0] = pir_op_val(ssp->ids[sh], ssp->piece_ty);
                            ex.results[0]  = pir_fn_new_val(mod, fn, "vc_e", sel_ty, b, PIR_INVALID_ID, 0u);
                            pir_fn_append_inst(fn, &new_insts, ex);
                            src_lane_v = ex.results[0];
                        }

                        PirInst cv = pir_make_inst(copy.op, del_ty, ln, lc);
                        cv.flags = copy.flags;
                        cv.operand_count = 1u; cv.operands = pir_alloc_ops(mod, 1u);
                        cv.operands[0] = pir_op_val(src_lane_v, sel_ty);
                        cv.results[0]  = (rsp && max_bits == 0u)
                                         ? rsp->ids[l]
                                         : pir_fn_new_val(mod, fn, "vc_c", del_ty, b, PIR_INVALID_ID, 0u);
                        pir_fn_append_inst(fn, &new_insts, cv);

                        if (rsp && max_bits == 0u) {
                            continue;
                        }
                        if (rsp == NULL) {
                            if (l == 0u) {
                                PirInst sp = pir_make_inst(PIR_OP_SPLAT, copy.ty, ln, lc);
                                sp.operand_count = 1u; sp.operands = pir_alloc_ops(mod, 1u);
                                sp.operands[0] = pir_op_val(cv.results[0], del_ty);
                                sp.results[0]  = (lanes == 1u) ? r0
                                                               : pir_fn_new_val(mod, fn, "vc_v", copy.ty, b, PIR_INVALID_ID, 0u);
                                pir_fn_append_inst(fn, &new_insts, sp);
                                dst_cur[0] = sp.results[0];
                            } else {
                                PirInst ins = pir_make_inst(PIR_OP_INSERT, copy.ty, ln, lc);
                                ins.imm_u64 = l;
                                ins.operand_count = 2u; ins.operands = pir_alloc_ops(mod, 2u);
                                ins.operands[0] = pir_op_val(dst_cur[0], copy.ty);
                                ins.operands[1] = pir_op_val(cv.results[0], del_ty);
                                ins.results[0]  = (l + 1u == lanes) ? r0
                                                                    : pir_fn_new_val(mod, fn, "vc_v", copy.ty, b, PIR_INVALID_ID, 0u);
                                pir_fn_append_inst(fn, &new_insts, ins);
                                dst_cur[0] = ins.results[0];
                            }
                        } else {
                            uint32_t dh = l / half_lanes;
                            uint32_t dl = l % half_lanes;
                            if (dl == 0u) {
                                PirInst sp = pir_make_inst(PIR_OP_SPLAT, rsp->piece_ty, ln, lc);
                                sp.operand_count = 1u; sp.operands = pir_alloc_ops(mod, 1u);
                                sp.operands[0] = pir_op_val(cv.results[0], del_ty);
                                sp.results[0]  = (half_lanes == 1u) ? rsp->ids[dh]
                                                                    : pir_fn_new_val(mod, fn, "vc_h", rsp->piece_ty, b, PIR_INVALID_ID, 0u);
                                pir_fn_append_inst(fn, &new_insts, sp);
                                dst_cur[dh] = sp.results[0];
                            } else {
                                PirInst ins = pir_make_inst(PIR_OP_INSERT, rsp->piece_ty, ln, lc);
                                ins.imm_u64 = dl;
                                ins.operand_count = 2u; ins.operands = pir_alloc_ops(mod, 2u);
                                ins.operands[0] = pir_op_val(dst_cur[dh], rsp->piece_ty);
                                ins.operands[1] = pir_op_val(cv.results[0], del_ty);
                                ins.results[0]  = (dl + 1u == half_lanes) ? rsp->ids[dh]
                                                                          : pir_fn_new_val(mod, fn, "vc_h", rsp->piece_ty, b, PIR_INVALID_ID, 0u);
                                pir_fn_append_inst(fn, &new_insts, ins);
                                dst_cur[dh] = ins.results[0];
                            }
                        }
                    }
                }
            } else if (copy.op == PIR_OP_RET && ret_is_ov) {
                /* Store split return vector pieces into hidden sret/out pointer, then void ret. */
                if (copy.operand_count < 1u || copy.operands[0].kind != PIR_OPER_VAL ||
                    copy.operands[0].u.val >= orig_vals ||
                    splits[copy.operands[0].u.val].count == 0u) {
                    pir_diag_error(diag, ln, 0u,
                                   "mismatched oversized vector operand in ret");
                    PIR_VEC_FREE(&new_insts);
                    PIR_VEC_FREE(&entry_allocas);
                    PIR_VEC_FREE(&aliases);
                    free(splits);
                    return false;
                }
                const VecSplit *sp = &splits[copy.operands[0].u.val];
                uint64_t pbytes = pir_type_bits(sp->piece_ty, target->ptr_bytes) / 8u;
                for (uint32_t p = 0u; p < sp->count; ++p) {
                    PirValueId dp = emit_padd(mod, fn, b, &new_insts,
                                              pir_op_val(ret_out_val, tptr),
                                              (uint64_t)p * pbytes, idx_ty, ln, lc);
                    PirInst st = pir_make_inst(PIR_OP_STORE, sp->piece_ty, ln, lc);
                    st.align = (uint32_t)(pbytes > 0u ? pbytes : 1u);
                    st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
                    st.operands[0] = pir_op_val(sp->ids[p], sp->piece_ty);
                    st.operands[1] = pir_op_val(dp, tptr);
                    pir_fn_append_inst(fn, &new_insts, st);
                }
                PirInst *slot = &fn->insts.data[iid];
                slot->ty            = tvoid;
                slot->operand_count = 0u;
                slot->operands      = NULL;
                PIR_VEC_PUSH(&new_insts, iid);
            } else if (copy.op == PIR_OP_CALL || copy.op == PIR_OP_CALL_INDIRECT) {
                /* Lower oversized vector return via temporary stack buffer + hidden arg 0,
                 * and split oversized vector arguments matching expanded callee params. */
                bool call_ret_ov = needs_split(copy.ty, max_bits);
                PirValueId ret_tmp = PIR_INVALID_ID;
                if (call_ret_ov) {
                    uint64_t tbytes = pir_vec_bits(copy.ty.u.vec) / 8u;
                    ret_tmp = pir_fn_emit_entry_alloca(mod, fn, "sv_ret",
                                                       tbytes, (uint32_t)tbytes,
                                                       ln, lc, &entry_allocas);
                }
                PIR_VEC(PirOperand) nargs = {0};
                PIR_VEC_PUSH(&nargs, copy.operands[0]);
                if (call_ret_ov) {
                    PIR_VEC_PUSH(&nargs, pir_op_val(ret_tmp, tptr));
                }
                for (uint32_t k = 1u; k < copy.operand_count; ++k) {
                    PirOperand a = copy.operands[k];
                    if (a.kind == PIR_OPER_VAL && a.u.val < orig_vals &&
                        splits[a.u.val].count > 0u) {
                        const VecSplit *sp = &splits[a.u.val];
                        for (uint32_t p = 0u; p < sp->count; ++p) {
                            PirOperand po = pir_op_val(sp->ids[p], sp->piece_ty);
                            po.call_stack_align  = (p == 0u) ? a.call_stack_align : 0u;
                            po.call_force_stack  = a.call_force_stack;
                            po.has_explicit_type = a.has_explicit_type;
                            PIR_VEC_PUSH(&nargs, po);
                        }
                    } else {
                        PIR_VEC_PUSH(&nargs, a);
                    }
                }
                PirInst *slot = &fn->insts.data[iid];
                slot->operand_count = nargs.len;
                slot->operands      = pir_alloc_ops(mod, nargs.len);
                memcpy(slot->operands, nargs.data, nargs.len * sizeof(PirOperand));
                PIR_VEC_FREE(&nargs);
                if (call_ret_ov) {
                    slot->ty         = tvoid;
                    slot->results[0] = PIR_INVALID_ID;
                }
                PIR_VEC_PUSH(&new_insts, iid);
                if (call_ret_ov && rsp) {
                    uint64_t pbytes = pir_type_bits(rsp->piece_ty, target->ptr_bytes) / 8u;
                    for (uint32_t p = 0u; p < rsp->count; ++p) {
                        PirValueId sp_ptr = emit_padd(mod, fn, b, &new_insts,
                                                      pir_op_val(ret_tmp, tptr),
                                                      (uint64_t)p * pbytes, idx_ty, ln, lc);
                        PirInst ld = pir_make_inst(PIR_OP_LOAD, rsp->piece_ty, ln, lc);
                        ld.align = (uint32_t)(pbytes > 0u ? pbytes : 1u);
                        ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
                        ld.operands[0] = pir_op_val(sp_ptr, tptr);
                        ld.results[0]  = rsp->ids[p];
                        pir_fn_append_inst(fn, &new_insts, ld);
                    }
                }
            } else {
                /* Generic lane-wise unary or binary op (arith, bitwise, shift, float). */
                if (!rsp || copy.op == PIR_OP_RET || copy.op == PIR_OP_INVOKE) {
                    pir_diag_error(diag, ln, 0u,
                                   "oversized vector exceeds target SIMD width (%u bits) in unsupported op",
                                   (unsigned)max_bits);
                    PIR_VEC_FREE(&new_insts);
                    PIR_VEC_FREE(&entry_allocas);
                    PIR_VEC_FREE(&aliases);
                    free(splits);
                    return false;
                }
                for (uint32_t k = 0u; k < copy.operand_count; ++k) {
                    if (copy.operands[k].kind != PIR_OPER_VAL ||
                        copy.operands[k].u.val >= orig_vals ||
                        splits[copy.operands[k].u.val].count != rsp->count) {
                        pir_diag_error(diag, ln, 0u,
                                       "mismatched oversized vector operand in SIMD legalization");
                        PIR_VEC_FREE(&new_insts);
                        PIR_VEC_FREE(&entry_allocas);
                        PIR_VEC_FREE(&aliases);
                        free(splits);
                        return false;
                    }
                }
                for (uint32_t p = 0u; p < rsp->count; ++p) {
                    PirInst ni = pir_make_inst(copy.op, rsp->piece_ty, ln, lc);
                    ni.flags         = copy.flags;
                    ni.operand_count = copy.operand_count;
                    ni.operands      = pir_alloc_ops(mod, copy.operand_count);
                    for (uint32_t k = 0u; k < copy.operand_count; ++k) {
                        const VecSplit *osp = &splits[copy.operands[k].u.val];
                        ni.operands[k] = pir_op_val(osp->ids[p], osp->piece_ty);
                    }
                    ni.results[0] = rsp->ids[p];
                    pir_fn_append_inst(fn, &new_insts, ni);
                }
            }
        }
        PIR_VEC_FREE(&fn->blocks.data[b].insts);
        fn->blocks.data[b].insts = new_insts;
    }

    pir_fn_prepend_entry_insts(fn, &entry_allocas);

    for (uint32_t i = 0u; i < aliases.len; ++i) {
        PirValueId root = aliases.data[i].to;
        uint32_t steps = 0u;
        for (bool progress = true; progress && steps <= aliases.len; ++steps) {
            progress = false;
            for (uint32_t k = 0u; k < aliases.len; ++k) {
                if (aliases.data[k].from == root) {
                    if (aliases.data[k].to == aliases.data[i].from) {
                        progress = false;
                        break;
                    }
                    root = aliases.data[k].to;
                    progress = true;
                    break;
                }
            }
        }
        if (root != aliases.data[i].from) {
            pir_fn_replace_val_uses(fn, aliases.data[i].from, root);
        }
    }
    PIR_VEC_FREE(&aliases);
    free(splits);
    return true;
}

static void split_simd_decl(PirModule *mod, const PirTargetSpec *target, PirCallingConv cc,
                            PirParamDecl **params_io, uint32_t *count_io,
                            PirType *ret_ty_io, PirCabiRetInfo *cabi_ret_io,
                            uint16_t max_bits) {
    PirParamDecl *old_p = *params_io;
    uint32_t old_n      = *count_io;
    bool ret_is_ov      = needs_split(*ret_ty_io, max_bits);
    bool has_any        = ret_is_ov;
    for (uint32_t i = 0u; i < old_n && !has_any; ++i) {
        if (needs_split(old_p[i].type, max_bits)) { has_any = true; }
    }
    if (!has_any) return;
    PIR_VEC(PirParamDecl) np = {0};
    if (ret_is_ov) {
        PirParamDecl op;
        memset(&op, 0, sizeof(op));
        op.type     = pir_type_scalar(PIR_SCALAR_PTR);
        op.abi_type = pir_type_scalar(PIR_SCALAR_VOID);
        op.val_id   = PIR_INVALID_ID;
        PIR_VEC_PUSH(&np, op);
        *ret_ty_io = pir_type_scalar(PIR_SCALAR_VOID);
        set_simd_sret_cabi(target, cc, cabi_ret_io);
    }
    for (uint32_t i = 0u; i < old_n; ++i) {
        if (needs_split(old_p[i].type, max_bits)) {
            PirType pty;
            uint32_t cnt = 0u;
            split_spec(old_p[i].type.u.vec, max_bits, &pty, &cnt);
            for (uint32_t p = 0u; p < cnt; ++p) {
                PirParamDecl pd = old_p[i];
                pd.type = pty;
                if (p > 0u) { pd.name = NULL; pd.val_id = PIR_INVALID_ID; pd.abi_stack_align = 0u; }
                PIR_VEC_PUSH(&np, pd);
            }
        } else {
            PIR_VEC_PUSH(&np, old_p[i]);
        }
    }
    *count_io  = np.len;
    *params_io = PIR_ARENA_NEW_ARRAY(&mod->arena, PirParamDecl, np.len);
    memcpy(*params_io, np.data, np.len * sizeof(PirParamDecl));
    PIR_VEC_FREE(&np);
}

bool pir_legalize_simd(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    if (target->caps.max_simd_bits >= 256u) {
        return true;
    }
    uint16_t max_bits = target->caps.max_simd_bits;
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        if (mod->funcs.data[fi].is_defined) {
            if (!legalize_simd_func(mod, target, fi, diag)) {
                return false;
            }
        } else {
            split_simd_decl(mod, target, mod->funcs.data[fi].cc,
                            &mod->funcs.data[fi].params, &mod->funcs.data[fi].param_count,
                            &mod->funcs.data[fi].ret_type, &mod->funcs.data[fi].cabi_ret,
                            max_bits);
        }
    }
    for (uint32_t si = 0u; si < mod->sigs.len; ++si) {
        split_simd_decl(mod, target, mod->sigs.data[si].cc,
                        &mod->sigs.data[si].params, &mod->sigs.data[si].param_count,
                        &mod->sigs.data[si].ret_type, &mod->sigs.data[si].cabi_ret,
                        max_bits);
    }

    /* Flatten oversized typed vector globals (> max_simd_bits) into raw byte/zeroinit data. */
    for (uint32_t gi = 0u; gi < mod->globals.len; ++gi) {
        PirGlobal *g = &mod->globals.data[gi];
        if (!g->has_type || g->type.kind != PIR_TYPE_VECTOR) continue;
        if (pir_vec_bits(g->type.u.vec) <= max_bits) continue;
        PirVecType vt = g->type.u.vec;
        PirScalarType elem = pir_vec_elem_scalar(vt);
        uint32_t lanes = pir_vec_lanes(vt);
        uint32_t esz = pir_scalar_bits(elem, target->ptr_bytes) / 8u;
        uint32_t total_bytes = lanes * esz;
        if (g->align < total_bytes) g->align = total_bytes;
        if (g->linkage == PIR_LINK_EXTERN) {
            g->has_type    = true;
            g->type        = pir_type_scalar(PIR_SCALAR_I8);
            g->is_zeroinit = false;
            g->is_braced   = false;
            g->item_count  = 0u;
            g->items       = NULL;
            continue;
        }
        g->has_type = false;
        g->type     = pir_type_scalar(PIR_SCALAR_VOID);
        if (g->is_zeroinit || g->item_count == 0u ||
            (g->item_count == 1u && g->items[0].kind == PIR_GITEM_ZERO)) {
            g->is_zeroinit   = true;
            g->is_braced     = false;
            g->item_count    = 1u;
            g->items         = PIR_ARENA_NEW_ARRAY(&mod->arena, PirGlobalItem, 1u);
            g->items[0].kind = PIR_GITEM_ZERO;
            g->items[0].len  = total_bytes;
        } else {
            uint8_t *buf = PIR_ARENA_NEW_ARRAY(&mod->arena, uint8_t, total_bytes);
            memset(buf, 0, total_bytes);
            for (uint32_t l = 0u; l < lanes && l < g->item_count; ++l) {
                const PirGlobalItem *it = &g->items[l];
                uint64_t raw = 0u;
                if (it->kind == PIR_GITEM_INT) {
                    uint64_t v = it->int_neg ? (UINT64_C(0) - it->int_lo) : it->int_lo;
                    if (elem == PIR_SCALAR_F32) {
                        float fv = it->int_neg ? -(float)it->int_lo : (float)it->int_lo;
                        uint32_t fb = 0u;
                        memcpy(&fb, &fv, 4u);
                        raw = fb;
                    } else if (elem == PIR_SCALAR_F64) {
                        double dv = it->int_neg ? -(double)it->int_lo : (double)it->int_lo;
                        memcpy(&raw, &dv, 8u);
                    } else {
                        raw = v;
                    }
                } else if (it->kind == PIR_GITEM_FLOAT) {
                    if (elem == PIR_SCALAR_F32) {
                        raw = (uint32_t)it->float_bits;
                    } else {
                        raw = it->float_bits;
                    }
                }
                for (uint32_t k = 0u; k < esz; ++k) {
                    uint32_t sh = target->is_big_endian ? ((esz - 1u - k) * 8u) : (k * 8u);
                    buf[l * esz + k] = (uint8_t)((raw >> sh) & 0xFFu);
                }
            }
            g->is_braced      = true;
            g->item_count     = 1u;
            g->items          = PIR_ARENA_NEW_ARRAY(&mod->arena, PirGlobalItem, 1u);
            g->items[0].kind  = PIR_GITEM_BYTES;
            g->items[0].bytes = buf;
            g->items[0].len   = total_bytes;
        }
    }
    return true;
}
