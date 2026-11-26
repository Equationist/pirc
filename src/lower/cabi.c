/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lower/lower.h"
#include "target/abi.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    bool           has_retbuf;
    PirType        retbuf_agg_ty;
    PirAbiArgInfo  ret_info;
    PirAbiArgInfo *param_infos;
    uint32_t       orig_param_count;
    PirAbiRegState end_reg_state;
    bool           needs_rewrite;
} CabiPlan;

static bool build_plan(const PirModule *mod, const PirTargetSpec *target,
                       PirCallingConv cc, const PirParamDecl *params,
                       uint32_t param_count, PirDiag *diag, uint32_t line,
                       CabiPlan *plan) {
    memset(plan, 0, sizeof(*plan));
    plan->orig_param_count = param_count;
    if (cc != PIR_CC_C) return true;

    if (target->arch == PIR_ARCH_CUSTOM) {
        bool has_special = false;
        for (uint32_t i = 0u; i < param_count; ++i) {
            if (params[i].abi_kind != PIR_PARAM_NORMAL ||
                (params[i].type.kind == PIR_TYPE_SCALAR &&
                 params[i].type.u.scalar == PIR_SCALAR_I128)) {
                has_special = true;
                break;
            }
        }
        if (!has_special) return true;
    }

    PirAbiRegState st = { 0u, 0u };
    uint32_t start_p = 0u;

    if (param_count > 0u && params[0].abi_kind == PIR_PARAM_RETBUF) {
        plan->has_retbuf    = true;
        plan->retbuf_agg_ty = params[0].abi_type;
        plan->needs_rewrite = true;
        if (!pir_abi_classify_retbuf(mod, target, params[0].abi_type, &plan->ret_info)) {
            pir_diag_error(diag, line, 0u, "unsupported C ABI target for retbuf");
            return false;
        }
        if (plan->ret_info.kind == PIR_ABI_INDIRECT) {
            st.gp_used += plan->ret_info.gp_used;
        }
        start_p = 1u;
    }

    if (param_count > 0u) {
        plan->param_infos = (PirAbiArgInfo *)calloc(param_count, sizeof(PirAbiArgInfo));
        if (!plan->param_infos) abort();
    }
    for (uint32_t i = start_p; i < param_count; ++i) {
        if (!pir_abi_classify_param(mod, target, &params[i], &st, &plan->param_infos[i])) {
            pir_diag_error(diag, line, 0u, "unsupported C ABI target for parameter %u", i);
            return false;
        }
        if (params[i].abi_kind != PIR_PARAM_NORMAL ||
            plan->param_infos[i].pad_gp_before > 0u ||
            plan->param_infos[i].pad_fp_before > 0u ||
            plan->param_infos[i].stack_align > 0u ||
            plan->param_infos[i].force_stack) {
            plan->needs_rewrite = true;
        }
    }
    plan->end_reg_state = st;
    return true;
}

static PirValueId cabi_padd(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                            PirOperand base, uint64_t off, PirType idx_ty, uint32_t ln, uint32_t lc) {
    if (off == 0u && base.kind == PIR_OPER_VAL) return base.u.val;
    PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
    pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
    pa.operands[0] = base; pa.operands[1] = pir_op_imm_i(off, false, idx_ty);
    pa.results[0]  = pir_fn_new_val(mod, fn, "abi_p", pir_type_scalar(PIR_SCALAR_PTR), blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, pa);
    return pa.results[0];
}

static uint64_t abi_reg_offset(const PirModule *mod, const PirTargetSpec *target,
                               const PirAbiArgInfo *info, uint32_t k) {
    if (info->has_reg_offsets) {
        return info->reg_offsets[k];
    }
    uint64_t off = 0u;
    for (uint32_t i = 0u; i < k; ++i) {
        off += pir_type_layout(mod, target, info->regs[i]).size;
    }
    return off;
}

static uint32_t reg_bundle_info(const PirModule *mod, const PirTargetSpec *target,
                                const PirAbiArgInfo *info, uint64_t *out_total, uint32_t *out_max_al) {
    uint64_t tot = 0u;
    uint32_t mal = 1u;
    for (uint32_t k = 0u; k < info->reg_count; ++k) {
        PirTypeLayout rl = pir_type_layout(mod, target, info->regs[k]);
        uint64_t end = abi_reg_offset(mod, target, info, k) + rl.size;
        if (end > tot) tot = end;
        if (rl.align > mal) mal = rl.align;
    }
    *out_total  = tot;
    *out_max_al = mal;
    return info->reg_count;
}

typedef PIR_VEC(PirOperand) PirOperandVec;

static bool agg_has_implicit_gaps(const PirModule *mod, const PirTargetSpec *target, PirType ty) {
    if (ty.kind != PIR_TYPE_NAMED_AGG || ty.u.agg_id >= mod->agg_types.len) return false;
    const PirAggDef *agg = &mod->agg_types.data[ty.u.agg_id];
    if (agg->kind == PIR_AGG_PAD || agg->kind == PIR_AGG_UNION || agg->kind == PIR_AGG_PACKED_STRUCT) {
        return false;
    }
    if (agg->kind == PIR_AGG_ARRAY) {
        if (agg->array_len == 0u) return false;
        PirTypeLayout el = pir_type_layout(mod, target, agg->elem_type);
        PirTypeLayout al = pir_type_layout(mod, target, ty);
        if (al.size > el.size * agg->array_len) return true;
        return agg_has_implicit_gaps(mod, target, agg->elem_type);
    }
    PirTypeLayout ly = pir_type_layout(mod, target, ty);
    uint64_t pos = 0u;
    for (uint32_t i = 0u; i < agg->field_count; ++i) {
        uint64_t foff = pir_field_offset(mod, target, ty.u.agg_id, i);
        if (foff > pos) return true;
        PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[i]);
        if (agg_has_implicit_gaps(mod, target, agg->fields[i])) return true;
        pos = foff + fl.size;
    }
    return pos < ly.size;
}

static bool agg_has_any_padding(const PirModule *mod, const PirTargetSpec *target, PirType ty) {
    if (ty.kind != PIR_TYPE_NAMED_AGG || ty.u.agg_id >= mod->agg_types.len) return false;
    const PirAggDef *agg = &mod->agg_types.data[ty.u.agg_id];
    if (agg->kind == PIR_AGG_PAD || agg->kind == PIR_AGG_UNION) return true;
    return agg_has_implicit_gaps(mod, target, ty);
}

static void emit_zero_range(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                            PirOperand base, uint64_t off, uint64_t len, PirType idx_ty,
                            uint32_t ln, uint32_t lc) {
    if (len == 0u) return;
    PirValueId dp = cabi_padd(mod, fn, blk, out, base, off, idx_ty, ln, lc);
    PirInst ms = pir_make_inst(PIR_OP_MEMSET, pir_type_scalar(PIR_SCALAR_VOID), ln, lc);
    ms.align = 1u; ms.operand_count = 3u; ms.operands = pir_alloc_ops(mod, 3u);
    ms.operands[0] = pir_op_val(dp, pir_type_scalar(PIR_SCALAR_PTR));
    ms.operands[1] = pir_op_imm_i(0u, false, pir_type_scalar(PIR_SCALAR_I8));
    ms.operands[2] = pir_op_imm_i(len, false, idx_ty);
    pir_fn_append_inst(fn, out, ms);
}

static void emit_zero_implicit_gaps(PirModule *mod, const PirTargetSpec *target,
                                    PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                                    PirOperand base, PirType ty, uint64_t base_off,
                                    PirType idx_ty, uint32_t ln, uint32_t lc) {
    if (ty.kind != PIR_TYPE_NAMED_AGG || ty.u.agg_id >= mod->agg_types.len) return;
    const PirAggDef *agg = &mod->agg_types.data[ty.u.agg_id];
    if (agg->kind == PIR_AGG_PAD || agg->kind == PIR_AGG_UNION || agg->kind == PIR_AGG_PACKED_STRUCT) {
        return;
    }
    PirTypeLayout ly = pir_type_layout(mod, target, ty);
    if (agg->kind == PIR_AGG_ARRAY) {
        PirTypeLayout el = pir_type_layout(mod, target, agg->elem_type);
        if (agg_has_implicit_gaps(mod, target, agg->elem_type)) {
            for (uint64_t i = 0u; i < agg->array_len; ++i) {
                emit_zero_implicit_gaps(mod, target, fn, blk, out, base, agg->elem_type,
                                        base_off + i * el.size, idx_ty, ln, lc);
            }
        }
        uint64_t used = el.size * agg->array_len;
        if (ly.size > used) {
            emit_zero_range(mod, fn, blk, out, base, base_off + used, ly.size - used, idx_ty, ln, lc);
        }
        return;
    }
    uint64_t pos = 0u;
    for (uint32_t i = 0u; i < agg->field_count; ++i) {
        uint64_t foff = pir_field_offset(mod, target, ty.u.agg_id, i);
        if (foff > pos) {
            emit_zero_range(mod, fn, blk, out, base, base_off + pos, foff - pos, idx_ty, ln, lc);
        }
        emit_zero_implicit_gaps(mod, target, fn, blk, out, base, agg->fields[i],
                                base_off + foff, idx_ty, ln, lc);
        PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[i]);
        pos = foff + fl.size;
    }
    if (ly.size > pos) {
        emit_zero_range(mod, fn, blk, out, base, base_off + pos, ly.size - pos, idx_ty, ln, lc);
    }
}

/* Caller-side packing of a byval(@T) pointer into either DIRECT regs or INDIRECT stack copy. */
static void pack_byval_arg(PirModule *mod, const PirTargetSpec *target,
                           PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                           PirOperand src_ptr, PirType agg_ty, const PirAbiArgInfo *info,
                           PirOperandVec *call_args, uint32_t ln, uint32_t lc) {
    PirType tptr   = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti8    = pir_type_scalar(PIR_SCALAR_I8);
    PirType idx_ty = pir_type_scalar(target->ptr_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);
    PirTypeLayout ly = pir_type_layout(mod, target, agg_ty);

    if (info->kind == PIR_ABI_INDIRECT) {
        /* Caller-side copy guarantees isolation (even when aliasing retbuf!). */
        PirInst cp = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, ln, lc);
        cp.imm_u64 = ly.size > 0u ? ly.size : 1u;
        cp.align   = ly.align > 0u ? ly.align : 1u;
        cp.results[0] = pir_fn_new_val(mod, fn, "byv_cp", tptr, blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, out, cp);

        PirInst mc = pir_make_inst(PIR_OP_MEMCPY, pir_type_scalar(PIR_SCALAR_VOID), ln, lc);
        mc.align = cp.align;
        mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
        mc.operands[0] = pir_op_val(cp.results[0], tptr);
        mc.operands[1] = src_ptr;
        mc.operands[2] = pir_op_imm_i(ly.size, false, idx_ty);
        pir_fn_append_inst(fn, out, mc);

        PirOperand ptr_op = pir_op_val(cp.results[0], tptr);
        ptr_op.call_byval_size  = info->stack_byval_size;
        ptr_op.call_byval_align = info->stack_byval_align;
        PIR_VEC_PUSH(call_args, ptr_op);
        return;
    }

    /* DIRECT: unpack into 1..4 scalar/vector registers. Use staging buffer if size < reg_bytes,
     * under-aligned, or aggregate contains padding bytes. */
    if (info->reg_count == 0u) return;
    uint64_t reg_bytes = 0u;
    uint32_t max_al    = 1u;
    reg_bundle_info(mod, target, info, &reg_bytes, &max_al);

    bool has_gap = agg_has_implicit_gaps(mod, target, agg_ty);
    PirOperand load_base = src_ptr;
    if (ly.size < reg_bytes || ly.align < max_al || has_gap) {
        PirInst stg = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, ln, lc);
        stg.imm_u64 = reg_bytes;
        stg.align   = max_al;
        stg.results[0] = pir_fn_new_val(mod, fn, "byv_st", tptr, blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, out, stg);

        if (ly.size < reg_bytes) {
            PirInst ms = pir_make_inst(PIR_OP_MEMSET, pir_type_scalar(PIR_SCALAR_VOID), ln, lc);
            ms.align = max_al; ms.operand_count = 3u; ms.operands = pir_alloc_ops(mod, 3u);
            ms.operands[0] = pir_op_val(stg.results[0], tptr);
            ms.operands[1] = pir_op_imm_i(0u, false, ti8);
            ms.operands[2] = pir_op_imm_i(reg_bytes, false, idx_ty);
            pir_fn_append_inst(fn, out, ms);
        }

        uint64_t copy_sz = (ly.size < reg_bytes) ? ly.size : reg_bytes;
        if (copy_sz > 0u) {
            PirInst mc = pir_make_inst(PIR_OP_MEMCPY, pir_type_scalar(PIR_SCALAR_VOID), ln, lc);
            mc.align = ly.align > 0u ? ly.align : 1u;
            mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
            mc.operands[0] = pir_op_val(stg.results[0], tptr);
            mc.operands[1] = src_ptr;
            mc.operands[2] = pir_op_imm_i(copy_sz, false, idx_ty);
            pir_fn_append_inst(fn, out, mc);
        }
        if (has_gap) {
            emit_zero_implicit_gaps(mod, target, fn, blk, out, pir_op_val(stg.results[0], tptr),
                                    agg_ty, 0u, idx_ty, ln, lc);
        }

        load_base = pir_op_val(stg.results[0], tptr);
    }

    for (uint32_t k = 0u; k < info->reg_count; ++k) {
        PirTypeLayout rl = pir_type_layout(mod, target, info->regs[k]);
        uint64_t off = abi_reg_offset(mod, target, info, k);
        PirValueId lp = cabi_padd(mod, fn, blk, out, load_base, off, idx_ty, ln, lc);
        PirInst ld = pir_make_inst(PIR_OP_LOAD, info->regs[k], ln, lc);
        ld.align = rl.align;
        ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
        ld.operands[0] = pir_op_val(lp, tptr);
        ld.results[0]  = pir_fn_new_val(mod, fn, "byv_r", info->regs[k], blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, out, ld);
        PirOperand vop = pir_op_val(ld.results[0], info->regs[k]);
        if (k == 0u && info->stack_align > 0u) {
            vop.call_stack_align = info->stack_align;
        }
        PIR_VEC_PUSH(call_args, vop);
    }
}

static void rewrite_call_site(PirModule *mod, const PirTargetSpec *target,
                              PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                              const PirInst *inst, const CabiPlan *plan,
                              const PirParamDecl *orig_params, bool is_variadic) {
    PirType tptr   = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti64   = pir_type_scalar(PIR_SCALAR_I64);
    PirType idx_ty = pir_type_scalar(target->ptr_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);
    uint32_t ln = inst->src_line, lc = inst->loc_idx;

    PirOperandVec new_ops = {0};
    PIR_VEC_PUSH(&new_ops, inst->operands[0]); /* callee symbol or fn_ptr */

    bool direct_retbuf = plan->has_retbuf && plan->ret_info.kind == PIR_ABI_DIRECT &&
                         plan->ret_info.reg_count <= 1u;
    PirOperand retbuf_out_ptr;
    memset(&retbuf_out_ptr, 0, sizeof(retbuf_out_ptr));

    uint32_t arg_start = 1u;
    if (plan->has_retbuf) {
        retbuf_out_ptr = inst->operands[1];
        if (!direct_retbuf) {
            PIR_VEC_PUSH(&new_ops, retbuf_out_ptr);
        }
        arg_start = 2u;
    }

    PirAbiRegState va_st = plan->end_reg_state;
    for (uint32_t i = arg_start; i < inst->operand_count; ++i) {
        uint32_t pi = i - 1u;
        PirOperand arg = inst->operands[i];
        if (pi < plan->orig_param_count) {
            const PirAbiArgInfo *info = &plan->param_infos[pi];
            for (uint32_t p = 0u; p < info->pad_gp_before; ++p) {
                PIR_VEC_PUSH(&new_ops, pir_op_imm_i(0u, false, ti64));
            }
            for (uint32_t p = 0u; p < info->pad_fp_before; ++p) {
                PIR_VEC_PUSH(&new_ops, pir_op_imm_f(0u, pir_type_scalar(PIR_SCALAR_F64)));
            }
            if (orig_params[pi].abi_kind == PIR_PARAM_BYVAL) {
                pack_byval_arg(mod, target, fn, blk, out, arg, orig_params[pi].abi_type,
                               info, &new_ops, ln, lc);
            } else {
                if (info->stack_align > 0u) {
                    arg.call_stack_align = info->stack_align;
                }
                PIR_VEC_PUSH(&new_ops, arg);
            }
        } else if (is_variadic && arg.call_abi == PIR_PARAM_BYVAL) {
            /* Variadic byval(@T) argument at call site (e.g. printf(..., %p: byval(@Point))). */
            PirParamDecl vpd;
            memset(&vpd, 0, sizeof(vpd));
            vpd.abi_kind = PIR_PARAM_BYVAL;
            vpd.abi_type = arg.call_abi_type;
            vpd.type     = tptr;
            PirAbiArgInfo vinfo;
            pir_abi_classify_param(mod, target, &vpd, &va_st, &vinfo);
            if (target->arch == PIR_ARCH_ARM64 && target->os == PIR_OS_MACOS &&
                vinfo.kind == PIR_ABI_DIRECT) {
                PirTypeLayout vly = pir_type_layout(mod, target, vpd.abi_type);
                if (vinfo.fp_used > 0u &&
                    vinfo.regs[0].kind == PIR_TYPE_SCALAR &&
                    vinfo.regs[0].u.scalar == PIR_SCALAR_F32) {
                    /* Darwin ARM64 passes all variadic args on 8B stack slots without promoting
                     * aggregate f32 members to f64; pack raw aggregate bytes into i64 words. */
                    uint32_t nw = (vly.size <= 8u) ? 1u : 2u;
                    vinfo.reg_count = nw;
                    vinfo.fp_used   = 0u;
                    vinfo.gp_used   = nw;
                    for (uint32_t w = 0u; w < nw; ++w) {
                        vinfo.regs[w] = ti64;
                    }
                }
                if (vly.align > 8u) {
                    vinfo.stack_align = (vly.align > 16u) ? 16u : vly.align;
                }
            } else if (target->arch == PIR_ARCH_X86_64 && target->os != PIR_OS_WINDOWS &&
                       vinfo.kind == PIR_ABI_DIRECT && vinfo.fp_used > 0u) {
                /* SysV AMD64: variadic aggregate f32 eightbytes must NOT undergo C default
                 * float->double promotion (cvtss2sd). Widen f32 pieces to v2f32 (or f64 when
                 * SIMD is disabled) so raw 32-bit float bits land in low XMM lane zero-padded. */
                PirType widen_ty = (target->caps.max_simd_bits >= 64u)
                                   ? pir_type_vec(PIR_VEC_V2F32)
                                   : pir_type_scalar(PIR_SCALAR_F64);
                for (uint32_t k = 0u; k < vinfo.reg_count; ++k) {
                    if (vinfo.regs[k].kind == PIR_TYPE_SCALAR &&
                        vinfo.regs[k].u.scalar == PIR_SCALAR_F32) {
                        vinfo.regs[k] = widen_ty;
                    }
                }
            } else if (target->arch == PIR_ARCH_ARM64 && target->os != PIR_OS_MACOS) {
                for (uint32_t p = 0u; p < vinfo.pad_gp_before; ++p) {
                    PIR_VEC_PUSH(&new_ops, pir_op_imm_i(0u, false, ti64));
                }
                for (uint32_t p = 0u; p < vinfo.pad_fp_before; ++p) {
                    PIR_VEC_PUSH(&new_ops, pir_op_imm_f(0u, pir_type_scalar(PIR_SCALAR_F64)));
                }
            }
            arg.call_abi          = PIR_PARAM_NORMAL;
            arg.has_explicit_type = false;
            pack_byval_arg(mod, target, fn, blk, out, arg, vpd.abi_type, &vinfo, &new_ops, ln, lc);
        } else {
            if (is_variadic) {
                PirParamDecl vpd;
                memset(&vpd, 0, sizeof(vpd));
                vpd.abi_kind = PIR_PARAM_NORMAL;
                vpd.type     = arg.type;
                PirAbiArgInfo vinfo;
                pir_abi_classify_param(mod, target, &vpd, &va_st, &vinfo);
                if (target->arch == PIR_ARCH_ARM64 && target->os == PIR_OS_MACOS) {
                    if ((arg.type.kind == PIR_TYPE_SCALAR && arg.type.u.scalar == PIR_SCALAR_I128) ||
                        (arg.type.kind == PIR_TYPE_VECTOR && pir_vec_bits(arg.type.u.vec) >= 128u)) {
                        arg.call_stack_align = 16u;
                    }
                } else {
                    for (uint32_t p = 0u; p < vinfo.pad_gp_before; ++p) {
                        PIR_VEC_PUSH(&new_ops, pir_op_imm_i(0u, false, ti64));
                    }
                    for (uint32_t p = 0u; p < vinfo.pad_fp_before; ++p) {
                        PIR_VEC_PUSH(&new_ops, pir_op_imm_f(0u, pir_type_scalar(PIR_SCALAR_F64)));
                    }
                    if (vinfo.force_stack) {
                        arg.call_force_stack = true;
                    }
                    if (vinfo.stack_align > 0u) {
                        arg.call_stack_align = vinfo.stack_align;
                    }
                }
            }
            PIR_VEC_PUSH(&new_ops, arg);
        }
    }

    PirInst call = *inst;
    call.operand_count = new_ops.len;
    call.operands      = pir_alloc_ops(mod, new_ops.len);
    memcpy(call.operands, new_ops.data, new_ops.len * sizeof(PirOperand));
    PIR_VEC_FREE(&new_ops);

    if (direct_retbuf && plan->ret_info.reg_count == 0u) {
        call.ty         = pir_type_scalar(PIR_SCALAR_VOID);
        call.results[0] = PIR_INVALID_ID;
        pir_fn_append_inst(fn, out, call);
    } else if (direct_retbuf) {
        PirType rty = plan->ret_info.regs[0];
        uint64_t roff = abi_reg_offset(mod, target, &plan->ret_info, 0u);
        call.ty         = rty;
        call.results[0] = pir_fn_new_val(mod, fn, "rb_rv", rty, blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, out, call);

        PirTypeLayout ly = pir_type_layout(mod, target, plan->retbuf_agg_ty);
        PirTypeLayout rl = pir_type_layout(mod, target, rty);
        if (ly.size >= roff + rl.size && ly.align >= rl.align && (roff % rl.align) == 0u) {
            PirValueId dp = cabi_padd(mod, fn, blk, out, retbuf_out_ptr, roff, idx_ty, ln, lc);
            PirInst st = pir_make_inst(PIR_OP_STORE, rty, ln, lc);
            st.align = rl.align; st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
            st.operands[0] = pir_op_val(call.results[0], rty);
            st.operands[1] = pir_op_val(dp, tptr);
            pir_fn_append_inst(fn, out, st);
        } else {
            PirInst stg = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, ln, lc);
            stg.imm_u64 = rl.size; stg.align = rl.align;
            stg.results[0] = pir_fn_new_val(mod, fn, "rb_st", tptr, blk, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, out, stg);

            PirInst st = pir_make_inst(PIR_OP_STORE, rty, ln, lc);
            st.align = rl.align; st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
            st.operands[0] = pir_op_val(call.results[0], rty);
            st.operands[1] = pir_op_val(stg.results[0], tptr);
            pir_fn_append_inst(fn, out, st);

            uint64_t copy_sz = (ly.size > roff) ? (ly.size - roff) : 0u;
            if (copy_sz > rl.size) copy_sz = rl.size;
            if (copy_sz > 0u) {
                PirValueId dp = cabi_padd(mod, fn, blk, out, retbuf_out_ptr, roff, idx_ty, ln, lc);
                PirInst mc = pir_make_inst(PIR_OP_MEMCPY, pir_type_scalar(PIR_SCALAR_VOID), ln, lc);
                mc.align = 1u;
                mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
                mc.operands[0] = pir_op_val(dp, tptr);
                mc.operands[1] = pir_op_val(stg.results[0], tptr);
                mc.operands[2] = pir_op_imm_i(copy_sz, false, idx_ty);
                pir_fn_append_inst(fn, out, mc);
            }
        }
    } else {
        pir_fn_append_inst(fn, out, call);
    }
}

static void populate_cabi_ret(const PirModule *mod, const PirTargetSpec *target,
                              const CabiPlan *plan, PirCabiRetInfo *out) {
    memset(out, 0, sizeof(*out));
    if (!plan->has_retbuf) return;
    if (plan->ret_info.kind == PIR_ABI_DIRECT && plan->ret_info.reg_count <= 1u) {
        out->kind = PIR_CABI_RET_NONE;
        return;
    }
    if (plan->ret_info.kind == PIR_ABI_DIRECT && plan->ret_info.reg_count > 1u) {
        PirTypeLayout ly = pir_type_layout(mod, target, plan->retbuf_agg_ty);
        out->kind      = PIR_CABI_RET_MULTI_REG;
        out->reg_count = plan->ret_info.reg_count;
        for (uint32_t k = 0u; k < plan->ret_info.reg_count; ++k) {
            out->regs[k] = plan->ret_info.regs[k];
        }
        out->agg_size  = ly.size;
        out->agg_align = ly.align;
        return;
    }
    /* PIR_ABI_INDIRECT: on ARM64, hidden sret pointer is passed in x8;
     * on x86-64 (SysV and Win64), hidden sret pointer is returned in rax. */
    if (target->arch == PIR_ARCH_ARM64) {
        out->kind = PIR_CABI_RET_SRET_X8;
    } else if (target->arch == PIR_ARCH_X86_64) {
        out->kind = PIR_CABI_RET_SRET_RAX;
    }
}

static void rewrite_param_decls(PirModule *mod, const PirTargetSpec *target, const CabiPlan *plan,
                                PirParamDecl **params_io, uint32_t *count_io,
                                PirType *ret_ty_io, PirCabiRetInfo *cabi_ret_io,
                                PirFunction *defined_fn) {
    PirParamDecl *old_p = *params_io;
    uint32_t old_n      = *count_io;
    PirType tptr  = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti64  = pir_type_scalar(PIR_SCALAR_I64);
    PirType tf64  = pir_type_scalar(PIR_SCALAR_F64);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);

    populate_cabi_ret(mod, target, plan, cabi_ret_io);

    PIR_VEC(PirParamDecl) np = {0};
    bool direct_retbuf = plan->has_retbuf && plan->ret_info.kind == PIR_ABI_DIRECT &&
                         plan->ret_info.reg_count <= 1u;
    uint32_t start = 0u;
    if (plan->has_retbuf) {
        if (direct_retbuf) {
            *ret_ty_io = (plan->ret_info.reg_count == 1u) ? plan->ret_info.regs[0] : tvoid;
        } else {
            PirParamDecl sret = old_p[0];
            sret.abi_kind = PIR_PARAM_NORMAL;
            sret.abi_type = tvoid;
            sret.type     = tptr;
            PIR_VEC_PUSH(&np, sret);
        }
        start = 1u;
    }

    for (uint32_t i = start; i < old_n; ++i) {
        const PirAbiArgInfo *info = &plan->param_infos[i];
        for (uint32_t p = 0u; p < info->pad_gp_before; ++p) {
            PirParamDecl pad;
            memset(&pad, 0, sizeof(pad));
            pad.type     = ti64;
            pad.abi_type = tvoid;
            pad.val_id   = PIR_INVALID_ID;
            if (defined_fn) {
                pad.val_id = pir_fn_new_val(mod, defined_fn, "abi_pad", ti64,
                                            PIR_INVALID_ID, PIR_INVALID_ID, np.len);
                pad.name   = defined_fn->values.data[pad.val_id].name;
            }
            PIR_VEC_PUSH(&np, pad);
        }
        for (uint32_t p = 0u; p < info->pad_fp_before; ++p) {
            PirParamDecl pad;
            memset(&pad, 0, sizeof(pad));
            pad.type     = tf64;
            pad.abi_type = tvoid;
            pad.val_id   = PIR_INVALID_ID;
            if (defined_fn) {
                pad.val_id = pir_fn_new_val(mod, defined_fn, "abi_fpad", tf64,
                                            PIR_INVALID_ID, PIR_INVALID_ID, np.len);
                pad.name   = defined_fn->values.data[pad.val_id].name;
            }
            PIR_VEC_PUSH(&np, pad);
        }
        if (old_p[i].abi_kind == PIR_PARAM_BYVAL) {
            if (info->kind == PIR_ABI_INDIRECT) {
                PirParamDecl p = old_p[i];
                p.abi_kind        = PIR_PARAM_NORMAL;
                p.abi_type        = tvoid;
                p.type            = tptr;
                p.abi_byval_size  = info->stack_byval_size;
                p.abi_byval_align = info->stack_byval_align;
                PIR_VEC_PUSH(&np, p);
            } else {
                for (uint32_t k = 0u; k < info->reg_count; ++k) {
                    PirParamDecl rp;
                    memset(&rp, 0, sizeof(rp));
                    rp.type     = info->regs[k];
                    rp.abi_type = tvoid;
                    rp.val_id   = PIR_INVALID_ID;
                    if (k == 0u && info->stack_align > 0u) {
                        rp.abi_stack_align = info->stack_align;
                    }
                    if (defined_fn) {
                        rp.val_id = pir_fn_new_val(mod, defined_fn, "abi_a", info->regs[k],
                                                   PIR_INVALID_ID, PIR_INVALID_ID, np.len);
                        rp.name   = defined_fn->values.data[rp.val_id].name;
                    }
                    PIR_VEC_PUSH(&np, rp);
                }
            }
        } else {
            PirParamDecl p = old_p[i];
            if (info->stack_align > 0u) {
                p.abi_stack_align = info->stack_align;
            }
            if (info->force_stack) {
                p.abi_force_stack = true;
            }
            PIR_VEC_PUSH(&np, p);
        }
    }

    if (defined_fn) {
        for (uint32_t k = 0u; k < np.len; ++k) {
            if (np.data[k].val_id < defined_fn->values.len) {
                defined_fn->values.data[np.data[k].val_id].param_index = k;
            }
        }
    }

    *count_io = np.len;
    if (np.len > 0u) {
        *params_io = PIR_ARENA_NEW_ARRAY(&mod->arena, PirParamDecl, np.len);
        memcpy(*params_io, np.data, np.len * sizeof(PirParamDecl));
    } else {
        *params_io = NULL;
    }
    PIR_VEC_FREE(&np);
}

static void rewrite_defined_callee(PirModule *mod, const PirTargetSpec *target,
                                   PirFunction *fn, const CabiPlan *plan) {
    if (!plan->needs_rewrite) return;
    PirType tptr   = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti8    = pir_type_scalar(PIR_SCALAR_I8);
    PirType idx_ty = pir_type_scalar(target->ptr_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);
    uint32_t ln    = fn->src_line;

    /* Save original params before mutating fn->params. */
    PirParamDecl *old_params = fn->params;
    uint32_t old_count       = fn->param_count;

    rewrite_param_decls(mod, target, plan, &fn->params, &fn->param_count, &fn->ret_type, &fn->cabi_ret, fn);

    PirInstIdVec prologue = {0};
    bool direct_retbuf = plan->has_retbuf && plan->ret_info.kind == PIR_ABI_DIRECT &&
                         plan->ret_info.reg_count <= 1u;
    PirValueId retbuf_local = PIR_INVALID_ID;
    if (direct_retbuf) {
        PirTypeLayout ly = pir_type_layout(mod, target, plan->retbuf_agg_ty);
        uint64_t sz = ly.size > 0u ? ly.size : 1u;
        uint32_t al = ly.align > 0u ? ly.align : 1u;
        if (plan->ret_info.reg_count == 1u) {
            PirType rty = plan->ret_info.regs[0];
            uint64_t roff = abi_reg_offset(mod, target, &plan->ret_info, 0u);
            PirTypeLayout rl = pir_type_layout(mod, target, rty);
            if (roff + rl.size > sz) sz = roff + rl.size;
            if (rl.align > al) al = rl.align;
        }

        retbuf_local = old_params[0].val_id;
        fn->values.data[retbuf_local].def_block = 0u;
        PirInst al_i = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, ln, 0u);
        al_i.imm_u64    = sz;
        al_i.align      = al;
        al_i.results[0] = retbuf_local;
        pir_fn_append_inst(fn, &prologue, al_i);

        if (ly.size < sz || agg_has_any_padding(mod, target, plan->retbuf_agg_ty)) {
            PirInst ms = pir_make_inst(PIR_OP_MEMSET, pir_type_scalar(PIR_SCALAR_VOID), ln, 0u);
            ms.align = al; ms.operand_count = 3u; ms.operands = pir_alloc_ops(mod, 3u);
            ms.operands[0] = pir_op_val(retbuf_local, tptr);
            ms.operands[1] = pir_op_imm_i(0u, false, ti8);
            ms.operands[2] = pir_op_imm_i(sz, false, idx_ty);
            pir_fn_append_inst(fn, &prologue, ms);
        }
    }

    /* Unpack DIRECT byval register params into local allocas reusing original param ValueIds. */
    uint32_t new_p_idx = (plan->has_retbuf && !direct_retbuf) ? 1u : 0u;
    uint32_t old_start = plan->has_retbuf ? 1u : 0u;
    for (uint32_t i = old_start; i < old_count; ++i) {
        const PirAbiArgInfo *info = &plan->param_infos[i];
        new_p_idx += info->pad_gp_before + info->pad_fp_before;
        if (old_params[i].abi_kind == PIR_PARAM_BYVAL && info->kind == PIR_ABI_DIRECT) {
            uint64_t reg_bytes = 0u;
            uint32_t max_al    = 1u;
            reg_bundle_info(mod, target, info, &reg_bytes, &max_al);
            PirTypeLayout ly = pir_type_layout(mod, target, old_params[i].abi_type);
            uint64_t sz = reg_bytes > ly.size ? reg_bytes : ly.size;
            if (sz == 0u) sz = 1u;
            uint32_t al = max_al > ly.align ? max_al : ly.align;

            PirValueId loc_v = old_params[i].val_id;
            fn->values.data[loc_v].def_block = 0u;
            PirInst al_i = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, ln, 0u);
            al_i.imm_u64 = sz; al_i.align = al; al_i.results[0] = loc_v;
            pir_fn_append_inst(fn, &prologue, al_i);

            for (uint32_t k = 0u; k < info->reg_count; ++k) {
                PirTypeLayout rl = pir_type_layout(mod, target, info->regs[k]);
                uint64_t off = abi_reg_offset(mod, target, info, k);
                PirValueId sp = cabi_padd(mod, fn, 0u, &prologue, pir_op_val(loc_v, tptr), off, idx_ty, ln, 0u);
                PirInst st = pir_make_inst(PIR_OP_STORE, info->regs[k], ln, 0u);
                st.align = rl.align; st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
                st.operands[0] = pir_op_val(fn->params[new_p_idx + k].val_id, info->regs[k]);
                st.operands[1] = pir_op_val(sp, tptr);
                pir_fn_append_inst(fn, &prologue, st);
            }
            new_p_idx += info->reg_count;
        } else {
            new_p_idx++;
        }
    }

    if (prologue.len > 0u) {
        for (uint32_t j = 0u; j < fn->blocks.data[0].insts.len; ++j) {
            PIR_VEC_PUSH(&prologue, fn->blocks.data[0].insts.data[j]);
        }
        PIR_VEC_FREE(&fn->blocks.data[0].insts);
        fn->blocks.data[0].insts = prologue;
    }

    /* For direct retbuf: load return register from retbuf_local before every `ret`. */
    if (direct_retbuf && plan->ret_info.reg_count == 1u) {
        PirType rty = plan->ret_info.regs[0];
        uint64_t roff = abi_reg_offset(mod, target, &plan->ret_info, 0u);
        PirTypeLayout rl = pir_type_layout(mod, target, rty);
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            PirInstIdVec *bi = &fn->blocks.data[b].insts;
            if (bi->len == 0u) continue;
            PirInstId tid = bi->data[bi->len - 1u];
            if (fn->insts.data[tid].op != PIR_OP_RET) continue;
            uint32_t rln = fn->insts.data[tid].src_line;
            uint32_t rlc = fn->insts.data[tid].loc_idx;
            bi->len--; /* pop ret */

            PirValueId lp = cabi_padd(mod, fn, b, bi, pir_op_val(retbuf_local, tptr), roff, idx_ty, rln, rlc);
            PirInst ld = pir_make_inst(PIR_OP_LOAD, rty, rln, rlc);
            ld.align = rl.align; ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
            ld.operands[0] = pir_op_val(lp, tptr);
            ld.results[0]  = pir_fn_new_val(mod, fn, "rb_ret", rty, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, bi, ld);

            PirInst ret_i = pir_make_inst(PIR_OP_RET, rty, rln, rlc);
            ret_i.operand_count = 1u; ret_i.operands = pir_alloc_ops(mod, 1u);
            ret_i.operands[0] = pir_op_val(ld.results[0], rty);
            pir_fn_append_inst(fn, bi, ret_i);
        }
    }
}

bool pir_lower_cabi(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    /* 1. Compute ABI plans for all functions & signatures before mutating anything. */
    CabiPlan *fn_plans  = (CabiPlan *)calloc(mod->funcs.len + 1u, sizeof(CabiPlan));
    CabiPlan *sig_plans = (CabiPlan *)calloc(mod->sigs.len + 1u, sizeof(CabiPlan));
    if (!fn_plans || !sig_plans) abort();

    bool ok = true;
    for (uint32_t i = 0u; i < mod->funcs.len && ok; ++i) {
        const PirFunction *f = &mod->funcs.data[i];
        ok = build_plan(mod, target, f->cc, f->params, f->param_count, diag, f->src_line, &fn_plans[i]);
    }
    for (uint32_t i = 0u; i < mod->sigs.len && ok; ++i) {
        const PirSigDef *s = &mod->sigs.data[i];
        ok = build_plan(mod, target, s->cc, s->params, s->param_count, diag, s->src_line, &sig_plans[i]);
    }
    if (!ok) goto cleanup;

    /* 2. Rewrite all call / call_indirect sites. */
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        PirFunction *fn = &mod->funcs.data[fi];
        if (!fn->is_defined) continue;
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            PirInstIdVec out = {0};
            for (uint32_t j = 0u; j < fn->blocks.data[b].insts.len; ++j) {
                PirInstId iid = fn->blocks.data[b].insts.data[j];
                PirInst copy  = fn->insts.data[iid];
                bool has_va_special = false;
                if (copy.op == PIR_OP_CALL || copy.op == PIR_OP_CALL_INDIRECT) {
                    for (uint32_t k = 1u; k < copy.operand_count; ++k) {
                        if (copy.operands[k].call_abi == PIR_PARAM_BYVAL ||
                            (copy.operands[k].type.kind == PIR_TYPE_SCALAR &&
                             copy.operands[k].type.u.scalar == PIR_SCALAR_I128) ||
                            (copy.operands[k].type.kind == PIR_TYPE_VECTOR &&
                             pir_vec_bits(copy.operands[k].type.u.vec) >= 128u)) {
                            has_va_special = true;
                            break;
                        }
                    }
                }
                if (copy.op == PIR_OP_CALL && copy.operands[0].u.sym.is_func) {
                    PirFuncId cid = copy.operands[0].u.sym.id;
                    const PirFunction *cal = &mod->funcs.data[cid];
                    if (fn_plans[cid].needs_rewrite || (cal->is_variadic && has_va_special)) {
                        rewrite_call_site(mod, target, fn, b, &out, &copy, &fn_plans[cid],
                                          cal->params, cal->is_variadic);
                        continue;
                    }
                } else if (copy.op == PIR_OP_CALL_INDIRECT && copy.sig_id < mod->sigs.len) {
                    PirSigId sid = copy.sig_id;
                    const PirSigDef *sig = &mod->sigs.data[sid];
                    if (sig_plans[sid].needs_rewrite || (sig->is_variadic && has_va_special)) {
                        rewrite_call_site(mod, target, fn, b, &out, &copy, &sig_plans[sid],
                                          sig->params, sig->is_variadic);
                        continue;
                    }
                }
                PIR_VEC_PUSH(&out, iid);
            }
            PIR_VEC_FREE(&fn->blocks.data[b].insts);
            fn->blocks.data[b].insts = out;
        }
    }

    /* 3. Rewrite all function definitions & extern declarations. */
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        if (!fn_plans[i].needs_rewrite) continue;
        PirFunction *fn = &mod->funcs.data[i];
        if (fn->is_defined) {
            rewrite_defined_callee(mod, target, fn, &fn_plans[i]);
        } else {
            rewrite_param_decls(mod, target, &fn_plans[i], &fn->params, &fn->param_count,
                                &fn->ret_type, &fn->cabi_ret, NULL);
        }
    }

    /* 4. Rewrite all signature declarations. */
    for (uint32_t i = 0u; i < mod->sigs.len; ++i) {
        if (!sig_plans[i].needs_rewrite) continue;
        PirSigDef *s = &mod->sigs.data[i];
        rewrite_param_decls(mod, target, &sig_plans[i], &s->params, &s->param_count,
                            &s->ret_type, &s->cabi_ret, NULL);
    }

cleanup:
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) free(fn_plans[i].param_infos);
    for (uint32_t i = 0u; i < mod->sigs.len; ++i)  free(sig_plans[i].param_infos);
    free(fn_plans);
    free(sig_plans);
    return ok;
}
