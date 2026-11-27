/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lower/lower.h"
#include "target/abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_valist_type(PirType t) {
    return t.kind == PIR_TYPE_SCALAR && t.u.scalar == PIR_SCALAR_VALIST;
}

static PirValueId va_padd(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                          PirOperand base, uint64_t off, PirType idx_ty, uint32_t ln, uint32_t lc) {
    if (off == 0u && base.kind == PIR_OPER_VAL) return base.u.val;
    PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
    pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
    pa.operands[0] = base; pa.operands[1] = pir_op_imm_i(off, false, idx_ty);
    pa.results[0]  = pir_fn_new_val(mod, fn, "vap", pir_type_scalar(PIR_SCALAR_PTR), blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, pa);
    return pa.results[0];
}

/* Materialize a va_arg / va_arg_buf value from a resolved argument-slot pointer `slot_ptr`. */
static void emit_va_arg_materialize(PirModule *mod, const PirTargetSpec *target,
                                    PirFunction *fn, PirBlockId dst_blk, PirInstIdVec *out,
                                    const PirInst *orig, const PirVaArgPlan *plan,
                                    PirValueId slot_ptr) {
    PirType tptr   = pir_type_scalar(PIR_SCALAR_PTR);
    PirType tvoid  = pir_type_scalar(PIR_SCALAR_VOID);
    PirType idx_ty = pir_type_scalar(target->ptr_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);
    uint32_t ptr_al = target->ptr_bytes > 0u ? (uint32_t)target->ptr_bytes : 8u;
    uint32_t ln = orig->src_line, lc = orig->loc_idx;

    PirValueId src_ptr = slot_ptr;
    if (plan->is_indirect) {
        PirInst lp = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
        lp.align = ptr_al; lp.operand_count = 1u; lp.operands = pir_alloc_ops(mod, 1u);
        lp.operands[0] = pir_op_val(slot_ptr, tptr);
        lp.results[0]  = pir_fn_new_val(mod, fn, "vaindp", tptr, dst_blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, out, lp);
        src_ptr = lp.results[0];
    }

    if (plan->is_buf) {
        PirInst mc = pir_make_inst(PIR_OP_MEMCPY, tvoid, ln, lc);
        mc.align = plan->align < ptr_al ? plan->align : ptr_al;
        mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
        mc.operands[0] = orig->operands[0];
        mc.operands[1] = pir_op_val(src_ptr, tptr);
        mc.operands[2] = pir_op_imm_i(plan->size, false, idx_ty);
        pir_fn_append_inst(fn, out, mc);
    } else if (plan->promote_f32) {
        PirInst ld = pir_make_inst(PIR_OP_LOAD, plan->load_ty, ln, lc);
        ld.align = ptr_al < 8u ? ptr_al : 8u;
        ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
        ld.operands[0] = pir_op_val(src_ptr, tptr);
        ld.results[0]  = pir_fn_new_val(mod, fn, "vaf64", plan->load_ty, dst_blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, out, ld);

        PirInst ft = pir_make_inst(PIR_OP_FPTRUNC, plan->arg_ty, ln, lc);
        ft.operand_count = 1u; ft.operands = pir_alloc_ops(mod, 1u);
        ft.operands[0] = pir_op_val(ld.results[0], plan->load_ty);
        ft.results[0]  = orig->results[0];
        fn->values.data[orig->results[0]].def_block = dst_blk;
        pir_fn_append_inst(fn, out, ft);
    } else {
        PirValueId ld_ptr = src_ptr;
        if (target->is_big_endian && !plan->is_indirect && plan->size > 0u && plan->size < ptr_al) {
            /* Big-endian variadic sub-word ints (i8/i16) are promoted and right-aligned in 4B slot. */
            ld_ptr = va_padd(mod, fn, dst_blk, out, pir_op_val(src_ptr, tptr),
                             (uint64_t)(ptr_al - plan->size), idx_ty, ln, lc);
        }
        uint32_t ld_align = plan->is_indirect
                            ? (plan->align > 0u ? plan->align : 1u)
                            : (plan->align < ptr_al ? plan->align : ptr_al);
        PirInst ld = pir_make_inst(PIR_OP_LOAD, plan->arg_ty, ln, lc);
        ld.align = ld_align;
        ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
        ld.operands[0] = pir_op_val(ld_ptr, tptr);
        ld.results[0]  = orig->results[0];
        fn->values.data[orig->results[0]].def_block = dst_blk;
        pir_fn_append_inst(fn, out, ld);
    }
}

/* Split block `b` at instruction `j` for branched va_arg (SysV x64 / Linux AAPCS64),
 * returning the newly created continuation block holding remaining instructions. */
static PirBlockId split_block_tail(PirModule *mod, PirFunction *fn, PirBlockId b,
                                   uint32_t split_after_j, PirInstIdVec *head_insts,
                                   uint32_t ln, PirValueId *out_ptr_param) {
    PirBlockId cont = pir_fn_new_block(mod, fn, "va_cont", ln);
    if (out_ptr_param) {
        PirValueId arg_p = pir_fn_new_val(mod, fn, "va_p", pir_type_scalar(PIR_SCALAR_PTR),
                                          cont, PIR_INVALID_ID, 0u);
        PIR_VEC_PUSH(&fn->blocks.data[cont].params, arg_p);
        *out_ptr_param = arg_p;
    }

    /* Move remaining original instructions (after split_after_j) into cont. */
    for (uint32_t k = split_after_j + 1u; k < fn->blocks.data[b].insts.len; ++k) {
        PirInstId mid = fn->blocks.data[b].insts.data[k];
        PirInst *mi = &fn->insts.data[mid];
        if (mi->results[0] != PIR_INVALID_ID) fn->values.data[mi->results[0]].def_block = cont;
        if (mi->results[1] != PIR_INVALID_ID) fn->values.data[mi->results[1]].def_block = cont;
        PIR_VEC_PUSH(&fn->blocks.data[cont].insts, mid);
    }
    PIR_VEC_FREE(&fn->blocks.data[b].insts);
    fn->blocks.data[b].insts = *head_insts;
    head_insts->data = NULL; head_insts->len = 0u; head_insts->cap = 0u;
    return cont;
}

/* Load SysV va_list->overflow_arg_area (@8), align up if mem_align > 8, advance by mem_step,
 * store updated overflow_arg_area back, and return the aligned argument pointer. */
static PirValueId emit_sysv_oaa_consume(PirModule *mod, const PirTargetSpec *target,
                                        PirFunction *fn, PirBlockId blk, PirInstIdVec *insts,
                                        PirOperand ap, uint64_t mem_align, uint64_t mem_step,
                                        uint32_t ln, uint32_t lc) {
    PirType tptr = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti64 = pir_type_scalar(PIR_SCALAR_I64);

    PirValueId oaa_p = va_padd(mod, fn, blk, insts, ap, 8u, ti64, ln, lc);
    PirInst loaa = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
    loaa.align = 8u; loaa.operand_count = 1u; loaa.operands = pir_alloc_ops(mod, 1u);
    loaa.operands[0] = pir_op_val(oaa_p, tptr);
    loaa.results[0]  = pir_fn_new_val(mod, fn, "vaoaa", tptr, blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, insts, loaa);

    PirValueId cur_oaa = loaa.results[0];
    if (mem_align > 8u) {
        cur_oaa = pir_emit_align_ptr(mod, fn, blk, insts, cur_oaa, mem_align, target, ln, lc);
    }

    PirInst noaa = pir_make_inst(PIR_OP_PTR_ADD, ti64, ln, lc);
    noaa.operand_count = 2u; noaa.operands = pir_alloc_ops(mod, 2u);
    noaa.operands[0] = pir_op_val(cur_oaa, tptr);
    noaa.operands[1] = pir_op_imm_i(mem_step, false, ti64);
    noaa.results[0]  = pir_fn_new_val(mod, fn, "vanoaa", tptr, blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, insts, noaa);

    PirInst soaa = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
    soaa.align = 8u; soaa.operand_count = 2u; soaa.operands = pir_alloc_ops(mod, 2u);
    soaa.operands[0] = pir_op_val(noaa.results[0], tptr);
    soaa.operands[1] = pir_op_val(oaa_p, tptr);
    pir_fn_append_inst(fn, insts, soaa);
    return cur_oaa;
}

static PirBlockId lower_va_arg_sysv_agg(PirModule *mod, const PirTargetSpec *target,
                                        PirFunction *fn, PirBlockId b, uint32_t j,
                                        PirInstIdVec *head, const PirInst *inst) {
    PirType tptr  = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti32  = pir_type_scalar(PIR_SCALAR_I32);
    PirType ti64  = pir_type_scalar(PIR_SCALAR_I64);
    PirType ti8   = pir_type_scalar(PIR_SCALAR_I8);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);
    uint32_t ln = inst->src_line, lc = inst->loc_idx;

    PirOperand dst_buf = inst->operands[0];
    PirOperand ap      = inst->operands[1];
    PirVaArgPlan plan;
    pir_abi_plan_va_arg(mod, target, inst->aux_type, true, &plan);
    const PirAbiArgInfo info = plan.abi;

    if (plan.is_empty) {
        PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, NULL);
        PirInst jc = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jc.targets[0].target = cont_blk;
        pir_fn_append_inst(fn, &fn->blocks.data[b].insts, jc);
        return cont_blk;
    }
    bool use_regs = !plan.force_mem;

    PirBlockId reg_blk  = pir_fn_new_block(mod, fn, "va_reg", ln);
    PirBlockId mem_blk  = pir_fn_new_block(mod, fn, "va_mem", ln);
    PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, NULL);

    PirInstIdVec *cur = &fn->blocks.data[b].insts;
    PirValueId gp_off_p = PIR_INVALID_ID, gp_off_v = PIR_INVALID_ID;
    PirValueId fp_off_p = PIR_INVALID_ID, fp_off_v = PIR_INVALID_ID;

    if (!use_regs) {
        PirInst jm = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jm.targets[0].target = mem_blk;
        pir_fn_append_inst(fn, cur, jm);
        /* Keep reg_blk well-formed (unreachable jump to cont_blk) */
        PirInst jr0 = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jr0.targets[0].target = cont_blk;
        pir_fn_append_inst(fn, &fn->blocks.data[reg_blk].insts, jr0);
    } else {
        PirValueId cond_val = PIR_INVALID_ID;
        if (info.gp_used > 0u) {
            gp_off_p = va_padd(mod, fn, b, cur, ap, 0u, ti64, ln, lc);
            PirInst lg = pir_make_inst(PIR_OP_LOAD, ti32, ln, lc);
            lg.align = 4u; lg.operand_count = 1u; lg.operands = pir_alloc_ops(mod, 1u);
            lg.operands[0] = pir_op_val(gp_off_p, tptr);
            lg.results[0]  = pir_fn_new_val(mod, fn, "vagp", ti32, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, cur, lg);
            gp_off_v = lg.results[0];

            PirInst cg = pir_make_inst(PIR_OP_CMP_LE_U, ti32, ln, lc);
            cg.operand_count = 2u; cg.operands = pir_alloc_ops(mod, 2u);
            cg.operands[0] = pir_op_val(gp_off_v, ti32);
            cg.operands[1] = pir_op_imm_i(48u - (uint64_t)info.gp_used * 8u, false, ti32);
            cg.results[0]  = pir_fn_new_val(mod, fn, "vacg", ti8, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, cur, cg);
            cond_val = cg.results[0];
        }
        if (info.fp_used > 0u) {
            fp_off_p = va_padd(mod, fn, b, cur, ap, 4u, ti64, ln, lc);
            PirInst lf = pir_make_inst(PIR_OP_LOAD, ti32, ln, lc);
            lf.align = 4u; lf.operand_count = 1u; lf.operands = pir_alloc_ops(mod, 1u);
            lf.operands[0] = pir_op_val(fp_off_p, tptr);
            lf.results[0]  = pir_fn_new_val(mod, fn, "vafp", ti32, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, cur, lf);
            fp_off_v = lf.results[0];

            PirInst cf = pir_make_inst(PIR_OP_CMP_LE_U, ti32, ln, lc);
            cf.operand_count = 2u; cf.operands = pir_alloc_ops(mod, 2u);
            cf.operands[0] = pir_op_val(fp_off_v, ti32);
            cf.operands[1] = pir_op_imm_i(176u - (uint64_t)info.fp_used * 16u, false, ti32);
            cf.results[0]  = pir_fn_new_val(mod, fn, "vacf", ti8, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, cur, cf);
            if (cond_val == PIR_INVALID_ID) {
                cond_val = cf.results[0];
            } else {
                PirInst ba = pir_make_inst(PIR_OP_AND, ti8, ln, lc);
                ba.operand_count = 2u; ba.operands = pir_alloc_ops(mod, 2u);
                ba.operands[0] = pir_op_val(cond_val, ti8);
                ba.operands[1] = pir_op_val(cf.results[0], ti8);
                ba.results[0]  = pir_fn_new_val(mod, fn, "vacb", ti8, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, cur, ba);
                cond_val = ba.results[0];
            }
        }
        PirInst br = pir_make_inst(PIR_OP_BR, tvoid, ln, lc);
        br.operand_count = 1u; br.operands = pir_alloc_ops(mod, 1u);
        br.operands[0] = pir_op_val(cond_val, ti8);
        br.targets[0].target = reg_blk;
        br.targets[1].target = mem_blk;
        pir_fn_append_inst(fn, cur, br);

        /* reg_blk: assemble aggregate chunks from reg_save_area (@16) directly into dst_buf */
        PirInstIdVec *rb = &fn->blocks.data[reg_blk].insts;
        PirValueId rsa_p = va_padd(mod, fn, reg_blk, rb, ap, 16u, ti64, ln, lc);
        PirInst lrsa = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
        lrsa.align = 8u; lrsa.operand_count = 1u; lrsa.operands = pir_alloc_ops(mod, 1u);
        lrsa.operands[0] = pir_op_val(rsa_p, tptr);
        lrsa.results[0]  = pir_fn_new_val(mod, fn, "varsa", tptr, reg_blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, rb, lrsa);

        uint32_t gp_i = 0u, fp_i = 0u;
        for (uint32_t k = 0u; k < info.reg_count; ++k) {
            PirType rty = info.regs[k];
            bool is_sse = pir_type_is_float(rty) || rty.kind == PIR_TYPE_VECTOR;
            uint64_t dst_off = info.has_reg_offsets ? info.reg_offsets[k] : ((uint64_t)k * 8u);
            uint64_t rem     = (plan.size > dst_off) ? (plan.size - dst_off) : 0u;
            uint64_t reg_sz  = (rty.kind == PIR_TYPE_VECTOR)
                               ? (uint64_t)(pir_vec_bits(rty.u.vec) / 8u)
                               : 8u;
            uint64_t chunk   = (rem < reg_sz) ? rem : reg_sz;
            if (chunk == 0u) continue;

            PirValueId base_off32 = is_sse ? fp_off_v : gp_off_v;
            uint64_t   delta      = is_sse ? ((uint64_t)fp_i * 16u) : ((uint64_t)gp_i * 8u);
            if (is_sse) fp_i++; else gp_i++;

            PirInst zoff = pir_make_inst(PIR_OP_ZEXT, ti64, ln, lc);
            zoff.operand_count = 1u; zoff.operands = pir_alloc_ops(mod, 1u);
            zoff.operands[0] = pir_op_val(base_off32, ti32);
            zoff.results[0]  = pir_fn_new_val(mod, fn, "vao64", ti64, reg_blk, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, rb, zoff);

            PirValueId off64 = zoff.results[0];
            if (delta > 0u) {
                PirInst ad = pir_make_inst(PIR_OP_ADD, ti64, ln, lc);
                ad.operand_count = 2u; ad.operands = pir_alloc_ops(mod, 2u);
                ad.operands[0] = pir_op_val(off64, ti64);
                ad.operands[1] = pir_op_imm_i(delta, false, ti64);
                ad.results[0]  = pir_fn_new_val(mod, fn, "vaod", ti64, reg_blk, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, rb, ad);
                off64 = ad.results[0];
            }

            PirInst srcp = pir_make_inst(PIR_OP_PTR_ADD, ti64, ln, lc);
            srcp.operand_count = 2u; srcp.operands = pir_alloc_ops(mod, 2u);
            srcp.operands[0] = pir_op_val(lrsa.results[0], tptr);
            srcp.operands[1] = pir_op_val(off64, ti64);
            srcp.results[0]  = pir_fn_new_val(mod, fn, "varsp", tptr, reg_blk, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, rb, srcp);

            PirValueId dp = va_padd(mod, fn, reg_blk, rb, dst_buf, dst_off, ti64, ln, lc);
            PirInst mc = pir_make_inst(PIR_OP_MEMCPY, tvoid, ln, lc);
            mc.align = 1u; mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
            mc.operands[0] = pir_op_val(dp, tptr);
            mc.operands[1] = pir_op_val(srcp.results[0], tptr);
            mc.operands[2] = pir_op_imm_i(chunk, false, ti64);
            pir_fn_append_inst(fn, rb, mc);
        }

        if (info.gp_used > 0u) {
            PirInst ng = pir_make_inst(PIR_OP_ADD, ti32, ln, lc);
            ng.operand_count = 2u; ng.operands = pir_alloc_ops(mod, 2u);
            ng.operands[0] = pir_op_val(gp_off_v, ti32);
            ng.operands[1] = pir_op_imm_i((uint64_t)info.gp_used * 8u, false, ti32);
            ng.results[0]  = pir_fn_new_val(mod, fn, "vang", ti32, reg_blk, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, rb, ng);

            PirInst sg = pir_make_inst(PIR_OP_STORE, ti32, ln, lc);
            sg.align = 4u; sg.operand_count = 2u; sg.operands = pir_alloc_ops(mod, 2u);
            sg.operands[0] = pir_op_val(ng.results[0], ti32);
            sg.operands[1] = pir_op_val(gp_off_p, tptr);
            pir_fn_append_inst(fn, rb, sg);
        }
        if (info.fp_used > 0u) {
            PirInst nf = pir_make_inst(PIR_OP_ADD, ti32, ln, lc);
            nf.operand_count = 2u; nf.operands = pir_alloc_ops(mod, 2u);
            nf.operands[0] = pir_op_val(fp_off_v, ti32);
            nf.operands[1] = pir_op_imm_i((uint64_t)info.fp_used * 16u, false, ti32);
            nf.results[0]  = pir_fn_new_val(mod, fn, "vanf", ti32, reg_blk, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, rb, nf);

            PirInst sf = pir_make_inst(PIR_OP_STORE, ti32, ln, lc);
            sf.align = 4u; sf.operand_count = 2u; sf.operands = pir_alloc_ops(mod, 2u);
            sf.operands[0] = pir_op_val(nf.results[0], ti32);
            sf.operands[1] = pir_op_val(fp_off_p, tptr);
            pir_fn_append_inst(fn, rb, sf);
        }

        PirInst jr = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jr.targets[0].target = cont_blk;
        pir_fn_append_inst(fn, rb, jr);
    }

    /* mem_blk: load overflow_arg_area (@8), align up if align > 8, advance, copy to dst_buf */
    PirInstIdVec *mb = &fn->blocks.data[mem_blk].insts;
    PirValueId cur_oaa = emit_sysv_oaa_consume(mod, target, fn, mem_blk, mb, ap,
                                               plan.mem_align, plan.mem_step, ln, lc);
    if (plan.size > 0u) {
        PirInst mc = pir_make_inst(PIR_OP_MEMCPY, tvoid, ln, lc);
        mc.align = plan.align < 8u ? plan.align : 8u;
        mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
        mc.operands[0] = dst_buf;
        mc.operands[1] = pir_op_val(cur_oaa, tptr);
        mc.operands[2] = pir_op_imm_i(plan.size, false, ti64);
        pir_fn_append_inst(fn, mb, mc);
    }

    PirInst jm = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
    jm.targets[0].target = cont_blk;
    pir_fn_append_inst(fn, mb, jm);
    return cont_blk;
}

static PirBlockId lower_va_arg_sysv(PirModule *mod, const PirTargetSpec *target,
                                    PirFunction *fn, PirBlockId b, uint32_t j,
                                    PirInstIdVec *head, const PirInst *inst) {
    if (inst->op == PIR_OP_VA_ARG_BUF) {
        return lower_va_arg_sysv_agg(mod, target, fn, b, j, head, inst);
    }
    PirType tptr = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti32 = pir_type_scalar(PIR_SCALAR_I32);
    PirType ti64 = pir_type_scalar(PIR_SCALAR_I64);
    PirType ti8  = pir_type_scalar(PIR_SCALAR_I8);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);
    uint32_t ln = inst->src_line, lc = inst->loc_idx;

    PirVaArgPlan plan;
    pir_abi_plan_va_arg(mod, target, inst->ty, false, &plan);
    PirOperand ap = inst->operands[0];

    PirBlockId reg_blk = plan.force_mem ? PIR_INVALID_ID : pir_fn_new_block(mod, fn, "va_reg", ln);
    PirBlockId mem_blk = pir_fn_new_block(mod, fn, "va_mem", ln);
    PirValueId res_ptr = PIR_INVALID_ID;
    PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, &res_ptr);

    /* SysV va_list: { gp_offset: i32 @0, fp_offset: i32 @4, overflow_arg_area: ptr @8, reg_save_area: ptr @16 } */
    PirInstIdVec *cur = &fn->blocks.data[b].insts;
    if (plan.force_mem) {
        /* SysV AMD64 §3.5.7: 256-bit vectors are always passed in MEMORY (overflow_arg_area),
         * never in the 176-byte reg_save_area (which only holds 128-bit xmm0..xmm7). */
        PirInst jm0 = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jm0.targets[0].target = mem_blk;
        pir_fn_append_inst(fn, cur, jm0);
    } else {
        uint64_t off_field = plan.is_fp ? 4u : 0u;
        uint64_t limit     = plan.is_fp ? 176u : 48u;
        uint64_t reg_step  = plan.is_fp ? 16u : (plan.mem_step > 8u ? plan.mem_step : 8u);

        PirValueId off_p = va_padd(mod, fn, b, cur, ap, off_field, ti64, ln, lc);
        PirInst loff = pir_make_inst(PIR_OP_LOAD, ti32, ln, lc);
        loff.align = 4u; loff.operand_count = 1u; loff.operands = pir_alloc_ops(mod, 1u);
        loff.operands[0] = pir_op_val(off_p, tptr);
        loff.results[0]  = pir_fn_new_val(mod, fn, "vao", ti32, b, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, cur, loff);

        PirInst cmp = pir_make_inst(PIR_OP_CMP_LE_U, ti32, ln, lc);
        cmp.operand_count = 2u; cmp.operands = pir_alloc_ops(mod, 2u);
        cmp.operands[0] = pir_op_val(loff.results[0], ti32);
        cmp.operands[1] = pir_op_imm_i(limit - reg_step, false, ti32);
        cmp.results[0]  = pir_fn_new_val(mod, fn, "vac", ti8, b, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, cur, cmp);

        PirInst br = pir_make_inst(PIR_OP_BR, tvoid, ln, lc);
        br.operand_count = 1u; br.operands = pir_alloc_ops(mod, 1u);
        br.operands[0] = pir_op_val(cmp.results[0], ti8);
        br.targets[0].target = reg_blk;
        br.targets[1].target = mem_blk;
        pir_fn_append_inst(fn, cur, br);

        /* reg_blk: load reg_save_area (@16), add offset, update offset */
        PirInstIdVec *rb = &fn->blocks.data[reg_blk].insts;
        PirValueId rsa_p = va_padd(mod, fn, reg_blk, rb, ap, 16u, ti64, ln, lc);
        PirInst lrsa = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
        lrsa.align = 8u; lrsa.operand_count = 1u; lrsa.operands = pir_alloc_ops(mod, 1u);
        lrsa.operands[0] = pir_op_val(rsa_p, tptr);
        lrsa.results[0]  = pir_fn_new_val(mod, fn, "varsa", tptr, reg_blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, rb, lrsa);

        PirInst zoff = pir_make_inst(PIR_OP_ZEXT, ti64, ln, lc);
        zoff.operand_count = 1u; zoff.operands = pir_alloc_ops(mod, 1u);
        zoff.operands[0] = pir_op_val(loff.results[0], ti32);
        zoff.results[0]  = pir_fn_new_val(mod, fn, "vao64", ti64, reg_blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, rb, zoff);

        PirInst rp = pir_make_inst(PIR_OP_PTR_ADD, ti64, ln, lc);
        rp.operand_count = 2u; rp.operands = pir_alloc_ops(mod, 2u);
        rp.operands[0] = pir_op_val(lrsa.results[0], tptr);
        rp.operands[1] = pir_op_val(zoff.results[0], ti64);
        rp.results[0]  = pir_fn_new_val(mod, fn, "varp", tptr, reg_blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, rb, rp);

        PirInst noff = pir_make_inst(PIR_OP_ADD, ti32, ln, lc);
        noff.operand_count = 2u; noff.operands = pir_alloc_ops(mod, 2u);
        noff.operands[0] = pir_op_val(loff.results[0], ti32);
        noff.operands[1] = pir_op_imm_i(reg_step, false, ti32);
        noff.results[0]  = pir_fn_new_val(mod, fn, "vano", ti32, reg_blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, rb, noff);

        PirInst soff = pir_make_inst(PIR_OP_STORE, ti32, ln, lc);
        soff.align = 4u; soff.operand_count = 2u; soff.operands = pir_alloc_ops(mod, 2u);
        soff.operands[0] = pir_op_val(noff.results[0], ti32);
        soff.operands[1] = pir_op_val(off_p, tptr);
        pir_fn_append_inst(fn, rb, soff);

        PirInst jr = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jr.targets[0].target = cont_blk; jr.targets[0].arg_count = 1u;
        jr.targets[0].args = pir_alloc_ops(mod, 1u);
        jr.targets[0].args[0] = pir_op_val(rp.results[0], tptr);
        pir_fn_append_inst(fn, rb, jr);
    }

    /* mem_blk: load overflow_arg_area (@8), align if > 8B, advance by step */
    PirInstIdVec *mb = &fn->blocks.data[mem_blk].insts;
    PirValueId arg_mem = emit_sysv_oaa_consume(mod, target, fn, mem_blk, mb, ap,
                                               plan.mem_align, plan.mem_step, ln, lc);

    PirInst jm = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
    jm.targets[0].target = cont_blk; jm.targets[0].arg_count = 1u;
    jm.targets[0].args = pir_alloc_ops(mod, 1u);
    jm.targets[0].args[0] = pir_op_val(arg_mem, tptr);
    pir_fn_append_inst(fn, mb, jm);

    /* Prepend load to cont_blk. */
    PirInstIdVec prep = {0};
    emit_va_arg_materialize(mod, target, fn, cont_blk, &prep, inst, &plan, res_ptr);
    for (uint32_t k = 0u; k < fn->blocks.data[cont_blk].insts.len; ++k) {
        PIR_VEC_PUSH(&prep, fn->blocks.data[cont_blk].insts.data[k]);
    }
    PIR_VEC_FREE(&fn->blocks.data[cont_blk].insts);
    fn->blocks.data[cont_blk].insts = prep;
    return cont_blk;
}

static PirBlockId lower_va_arg_aapcs64_linux(PirModule *mod, const PirTargetSpec *target,
                                             PirFunction *fn, PirBlockId b, uint32_t j,
                                             PirInstIdVec *head, const PirInst *inst) {
    PirType tptr = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti32 = pir_type_scalar(PIR_SCALAR_I32);
    PirType ti64 = pir_type_scalar(PIR_SCALAR_I64);
    PirType ti8  = pir_type_scalar(PIR_SCALAR_I8);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);
    uint32_t ln = inst->src_line, lc = inst->loc_idx;

    bool is_buf = (inst->op == PIR_OP_VA_ARG_BUF);
    PirVaArgPlan plan;
    pir_abi_plan_va_arg(mod, target, is_buf ? inst->aux_type : inst->ty, is_buf, &plan);
    PirOperand ap = is_buf ? inst->operands[1] : inst->operands[0];

    if (plan.is_empty) {
        PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, NULL);
        PirInst jc = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jc.targets[0].target = cont_blk;
        pir_fn_append_inst(fn, &fn->blocks.data[b].insts, jc);
        return cont_blk;
    }

    PirBlockId reg_blk = pir_fn_new_block(mod, fn, "va_reg", ln);
    PirBlockId mem_blk = pir_fn_new_block(mod, fn, "va_mem", ln);
    PirValueId res_ptr = PIR_INVALID_ID;
    PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, plan.is_hfa ? NULL : &res_ptr);

    /* Linux AAPCS64 va_list: { __stack: ptr @0, __gr_top: ptr @8, __vr_top: ptr @16, __gr_offs: i32 @24, __vr_offs: i32 @28 } */
    uint64_t top_off  = plan.is_fp ? 16u : 8u;
    uint64_t offs_off = plan.is_fp ? 28u : 24u;
    uint64_t reg_step = plan.is_hfa ? ((uint64_t)plan.abi.fp_used * 16u)
                      : plan.is_fp  ? 16u
                                    : (plan.mem_step > 8u ? plan.mem_step : 8u);

    PirInstIdVec *cur = &fn->blocks.data[b].insts;
    PirValueId op = va_padd(mod, fn, b, cur, ap, offs_off, ti64, ln, lc);
    PirInst lo = pir_make_inst(PIR_OP_LOAD, ti32, ln, lc);
    lo.align = 4u; lo.operand_count = 1u; lo.operands = pir_alloc_ops(mod, 1u);
    lo.operands[0] = pir_op_val(op, tptr);
    lo.results[0]  = pir_fn_new_val(mod, fn, "vao", ti32, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, cur, lo);

    PirValueId cur_offs = lo.results[0];
    if (plan.align_gp16) {
        /* AAPCS64 Appendix B: 16B-aligned GP arg rounds __gr_offs up to a multiple of 16. */
        PirInst ad = pir_make_inst(PIR_OP_ADD, ti32, ln, lc);
        ad.operand_count = 2u; ad.operands = pir_alloc_ops(mod, 2u);
        ad.operands[0] = pir_op_val(cur_offs, ti32);
        ad.operands[1] = pir_op_imm_i(15u, false, ti32);
        ad.results[0]  = pir_fn_new_val(mod, fn, "vao_a", ti32, b, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, cur, ad);

        PirInst an = pir_make_inst(PIR_OP_AND, ti32, ln, lc);
        an.operand_count = 2u; an.operands = pir_alloc_ops(mod, 2u);
        an.operands[0] = pir_op_val(ad.results[0], ti32);
        an.operands[1] = pir_op_imm_i(16u, true, ti32); /* -16 = ~15 */
        an.results[0]  = pir_fn_new_val(mod, fn, "vao_m", ti32, b, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, cur, an);
        cur_offs = an.results[0];
    }

    PirInst no = pir_make_inst(PIR_OP_ADD, ti32, ln, lc);
    no.operand_count = 2u; no.operands = pir_alloc_ops(mod, 2u);
    no.operands[0] = pir_op_val(cur_offs, ti32);
    no.operands[1] = pir_op_imm_i(reg_step, false, ti32);
    no.results[0]  = pir_fn_new_val(mod, fn, "vano", ti32, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, cur, no);

    PirInst cmp = pir_make_inst(PIR_OP_CMP_LE_S, ti32, ln, lc);
    cmp.operand_count = 2u; cmp.operands = pir_alloc_ops(mod, 2u);
    cmp.operands[0] = pir_op_val(no.results[0], ti32);
    cmp.operands[1] = pir_op_imm_i(0u, false, ti32);
    cmp.results[0]  = pir_fn_new_val(mod, fn, "vac", ti8, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, cur, cmp);

    PirInst br = pir_make_inst(PIR_OP_BR, tvoid, ln, lc);
    br.operand_count = 1u; br.operands = pir_alloc_ops(mod, 1u);
    br.operands[0] = pir_op_val(cmp.results[0], ti8);
    br.targets[0].target = reg_blk;
    br.targets[1].target = mem_blk;
    pir_fn_append_inst(fn, cur, br);

    /* reg_blk */
    PirInstIdVec *rb = &fn->blocks.data[reg_blk].insts;
    PirInst so = pir_make_inst(PIR_OP_STORE, ti32, ln, lc);
    so.align = 4u; so.operand_count = 2u; so.operands = pir_alloc_ops(mod, 2u);
    so.operands[0] = pir_op_val(no.results[0], ti32);
    so.operands[1] = pir_op_val(op, tptr);
    pir_fn_append_inst(fn, rb, so);

    PirValueId tp = va_padd(mod, fn, reg_blk, rb, ap, top_off, ti64, ln, lc);
    PirInst lt = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
    lt.align = 8u; lt.operand_count = 1u; lt.operands = pir_alloc_ops(mod, 1u);
    lt.operands[0] = pir_op_val(tp, tptr);
    lt.results[0]  = pir_fn_new_val(mod, fn, "vatop", tptr, reg_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, rb, lt);

    PirInst sx = pir_make_inst(PIR_OP_SEXT, ti64, ln, lc);
    sx.operand_count = 1u; sx.operands = pir_alloc_ops(mod, 1u);
    sx.operands[0] = pir_op_val(cur_offs, ti32);
    sx.results[0]  = pir_fn_new_val(mod, fn, "vao64", ti64, reg_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, rb, sx);

    PirInst rp = pir_make_inst(PIR_OP_PTR_ADD, ti64, ln, lc);
    rp.operand_count = 2u; rp.operands = pir_alloc_ops(mod, 2u);
    rp.operands[0] = pir_op_val(lt.results[0], tptr);
    rp.operands[1] = pir_op_val(sx.results[0], ti64);
    rp.results[0]  = pir_fn_new_val(mod, fn, "varp", tptr, reg_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, rb, rp);

    if (plan.is_hfa) {
        /* Copy each HFA element from its 16-byte __vr_top slot into packed dst_buf. */
        PirType ety = plan.abi.regs[0];
        PirTypeLayout ely = pir_type_layout(mod, target, ety);
        for (uint32_t k = 0u; k < plan.abi.reg_count; ++k) {
            PirValueId sp = va_padd(mod, fn, reg_blk, rb, pir_op_val(rp.results[0], tptr),
                                    (uint64_t)k * 16u, ti64, ln, lc);
            PirInst ld = pir_make_inst(PIR_OP_LOAD, ety, ln, lc);
            ld.align = ely.align; ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
            ld.operands[0] = pir_op_val(sp, tptr);
            ld.results[0]  = pir_fn_new_val(mod, fn, "vahfa", ety, reg_blk, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, rb, ld);

            PirValueId dp = va_padd(mod, fn, reg_blk, rb, inst->operands[0],
                                    (uint64_t)k * ely.size, ti64, ln, lc);
            PirInst st = pir_make_inst(PIR_OP_STORE, ety, ln, lc);
            st.align = (plan.align > 0u && plan.align < ely.align) ? plan.align : ely.align;
            st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
            st.operands[0] = pir_op_val(ld.results[0], ety);
            st.operands[1] = pir_op_val(dp, tptr);
            pir_fn_append_inst(fn, rb, st);
        }
        PirInst jr = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jr.targets[0].target = cont_blk;
        pir_fn_append_inst(fn, rb, jr);
    } else {
        PirInst jr = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jr.targets[0].target = cont_blk; jr.targets[0].arg_count = 1u;
        jr.targets[0].args = pir_alloc_ops(mod, 1u);
        jr.targets[0].args[0] = pir_op_val(rp.results[0], tptr);
        pir_fn_append_inst(fn, rb, jr);
    }

    /* mem_blk: zero exhausted __gr_offs/__vr_offs per AAPCS64 Appendix B, align & advance __stack (@0) */
    PirInstIdVec *mb = &fn->blocks.data[mem_blk].insts;
    PirInst sz = pir_make_inst(PIR_OP_STORE, ti32, ln, lc);
    sz.align = 4u; sz.operand_count = 2u; sz.operands = pir_alloc_ops(mod, 2u);
    sz.operands[0] = pir_op_imm_i(0u, false, ti32);
    sz.operands[1] = pir_op_val(op, tptr);
    pir_fn_append_inst(fn, mb, sz);

    PirInst ls = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
    ls.align = 8u; ls.operand_count = 1u; ls.operands = pir_alloc_ops(mod, 1u);
    ls.operands[0] = ap;
    ls.results[0]  = pir_fn_new_val(mod, fn, "vastk", tptr, mem_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, mb, ls);

    PirValueId stk_ptr = ls.results[0];
    if (plan.mem_align > 8u) {
        stk_ptr = pir_emit_align_ptr(mod, fn, mem_blk, mb, stk_ptr, plan.mem_align, target, ln, lc);
    }

    PirInst ns = pir_make_inst(PIR_OP_PTR_ADD, ti64, ln, lc);
    ns.operand_count = 2u; ns.operands = pir_alloc_ops(mod, 2u);
    ns.operands[0] = pir_op_val(stk_ptr, tptr);
    ns.operands[1] = pir_op_imm_i(plan.mem_step, false, ti64);
    ns.results[0]  = pir_fn_new_val(mod, fn, "vanstk", tptr, mem_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, mb, ns);

    PirInst ss = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
    ss.align = 8u; ss.operand_count = 2u; ss.operands = pir_alloc_ops(mod, 2u);
    ss.operands[0] = pir_op_val(ns.results[0], tptr);
    ss.operands[1] = ap;
    pir_fn_append_inst(fn, mb, ss);

    if (plan.is_hfa) {
        PirInst mc = pir_make_inst(PIR_OP_MEMCPY, tvoid, ln, lc);
        mc.align = plan.align < 8u ? plan.align : 8u;
        mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
        mc.operands[0] = inst->operands[0];
        mc.operands[1] = pir_op_val(stk_ptr, tptr);
        mc.operands[2] = pir_op_imm_i(plan.size, false, ti64);
        pir_fn_append_inst(fn, mb, mc);

        PirInst jm = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jm.targets[0].target = cont_blk;
        pir_fn_append_inst(fn, mb, jm);
        return cont_blk;
    }

    PirInst jm = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
    jm.targets[0].target = cont_blk; jm.targets[0].arg_count = 1u;
    jm.targets[0].args = pir_alloc_ops(mod, 1u);
    jm.targets[0].args[0] = pir_op_val(stk_ptr, tptr);
    pir_fn_append_inst(fn, mb, jm);

    PirInstIdVec prep = {0};
    emit_va_arg_materialize(mod, target, fn, cont_blk, &prep, inst, &plan, res_ptr);
    for (uint32_t k = 0u; k < fn->blocks.data[cont_blk].insts.len; ++k) {
        PIR_VEC_PUSH(&prep, fn->blocks.data[cont_blk].insts.data[k]);
    }
    PIR_VEC_FREE(&fn->blocks.data[cont_blk].insts);
    fn->blocks.data[cont_blk].insts = prep;
    return cont_blk;
}

static void lower_va_arg_ptr_cursor(PirModule *mod, const PirTargetSpec *target,
                                    PirFunction *fn, PirBlockId b, PirInstIdVec *out,
                                    const PirInst *inst) {
    /* Darwin ARM64, Win64, & 32-bit pointer-cursor targets: va_list is a flat char * cursor. */
    PirType tptr   = pir_type_scalar(PIR_SCALAR_PTR);
    PirType idx_ty = pir_type_scalar(target->ptr_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);
    uint32_t ptr_al = target->ptr_bytes > 0u ? (uint32_t)target->ptr_bytes : 8u;
    uint32_t ln = inst->src_line, lc = inst->loc_idx;

    bool is_buf = (inst->op == PIR_OP_VA_ARG_BUF);
    PirVaArgPlan plan;
    pir_abi_plan_va_arg(mod, target, is_buf ? inst->aux_type : inst->ty, is_buf, &plan);
    if (plan.is_empty) return;
    PirOperand ap = is_buf ? inst->operands[1] : inst->operands[0];

    PirInst lcur = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
    lcur.align = ptr_al; lcur.operand_count = 1u; lcur.operands = pir_alloc_ops(mod, 1u);
    lcur.operands[0] = ap;
    lcur.results[0]  = pir_fn_new_val(mod, fn, "vacur", tptr, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, lcur);

    PirValueId cur_ptr = lcur.results[0];
    if (plan.mem_align > ptr_al) {
        cur_ptr = pir_emit_align_ptr(mod, fn, b, out, cur_ptr, plan.mem_align, target, ln, lc);
    }

    PirInst next = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
    next.operand_count = 2u; next.operands = pir_alloc_ops(mod, 2u);
    next.operands[0] = pir_op_val(cur_ptr, tptr);
    next.operands[1] = pir_op_imm_i(plan.mem_step, false, idx_ty);
    next.results[0]  = pir_fn_new_val(mod, fn, "vanxt", tptr, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, next);

    PirInst scur = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
    scur.align = ptr_al; scur.operand_count = 2u; scur.operands = pir_alloc_ops(mod, 2u);
    scur.operands[0] = pir_op_val(next.results[0], tptr);
    scur.operands[1] = ap;
    pir_fn_append_inst(fn, out, scur);

    emit_va_arg_materialize(mod, target, fn, b, out, inst, &plan, cur_ptr);
}

static PirTypeId ensure_aapcs64_valist_struct(PirModule *mod) {
    static const char *NAME = "__pir_aapcs64_va_list";
    for (uint32_t i = 0u; i < mod->agg_types.len; ++i) {
        if (mod->agg_types.data[i].name && strcmp(mod->agg_types.data[i].name, NAME) == 0) {
            return i;
        }
    }
    PirAggDef d;
    memset(&d, 0, sizeof(d));
    d.name        = pir_arena_strdup(&mod->arena, NAME);
    d.kind        = PIR_AGG_STRUCT;
    d.field_count = 5u;
    d.fields      = PIR_ARENA_NEW_ARRAY(&mod->arena, PirType, 5u);
    d.fields[0]   = pir_type_scalar(PIR_SCALAR_PTR);
    d.fields[1]   = pir_type_scalar(PIR_SCALAR_PTR);
    d.fields[2]   = pir_type_scalar(PIR_SCALAR_PTR);
    d.fields[3]   = pir_type_scalar(PIR_SCALAR_I32);
    d.fields[4]   = pir_type_scalar(PIR_SCALAR_I32);
    PirTypeId id  = mod->agg_types.len;
    PIR_VEC_PUSH(&mod->agg_types, d);
    return id;
}

static void va_split_vec_spec(PirVecType vt, uint16_t max_bits, PirType *out_piece, uint32_t *out_cnt) {
    PirScalarType elem = pir_vec_elem_scalar(vt);
    uint32_t lanes = pir_vec_lanes(vt);
    uint16_t bits  = pir_vec_bits(vt);
    if (max_bits < 64u || (max_bits == 64u && (elem == PIR_SCALAR_I64 || elem == PIR_SCALAR_F64))) {
        *out_piece = pir_type_scalar(elem);
        *out_cnt   = lanes;
        return;
    }
    if (max_bits == 128u && bits == 256u) {
        PirVecType half = PIR_VEC_V4I32;
        switch (vt) {
            case PIR_VEC_V32I8:  half = PIR_VEC_V16I8; break;
            case PIR_VEC_V16I16: half = PIR_VEC_V8I16; break;
            case PIR_VEC_V8I32:  half = PIR_VEC_V4I32; break;
            case PIR_VEC_V4I64:  half = PIR_VEC_V2I64; break;
            case PIR_VEC_V8F32:  half = PIR_VEC_V4F32; break;
            case PIR_VEC_V4F64:  half = PIR_VEC_V2F64; break;
            case PIR_VEC_NONE:
            case PIR_VEC_V8I8: case PIR_VEC_V4I16: case PIR_VEC_V2I32: case PIR_VEC_V2F32:
            case PIR_VEC_V16I8: case PIR_VEC_V8I16: case PIR_VEC_V4I32: case PIR_VEC_V2I64:
            case PIR_VEC_V4F32: case PIR_VEC_V2F64:
                half = vt;
                break;
        }
        *out_piece = pir_type_vec(half);
        *out_cnt   = 2u;
        return;
    }
    PirVecType v64 = PIR_VEC_V2I32;
    switch (elem) {
        case PIR_SCALAR_I8:  v64 = PIR_VEC_V8I8;  break;
        case PIR_SCALAR_I16: v64 = PIR_VEC_V4I16; break;
        case PIR_SCALAR_I32: v64 = PIR_VEC_V2I32; break;
        case PIR_SCALAR_F32: v64 = PIR_VEC_V2F32; break;
        case PIR_SCALAR_I64: case PIR_SCALAR_F64: case PIR_SCALAR_I128:
        case PIR_SCALAR_PTR: case PIR_SCALAR_VALIST: case PIR_SCALAR_VOID:
            *out_piece = pir_type_scalar(elem);
            *out_cnt   = lanes;
            return;
    }
    *out_piece = pir_type_vec(v64);
    *out_cnt   = (uint32_t)(bits / 64u);
}

/* SysV PPC32 va_list (12B): { uint8_t gpr @0, uint8_t fpr @1, uint16_t pad @2,
 *                             void *overflow_arg_area @4, void *reg_save_area @8 }.
 * reg_save_area layout: r3..r10 at [0..31] (4B each), f1..f8 at [32..95] (8B each). */
static void lower_va_arg_ppc32(PirModule *mod, const PirTargetSpec *target,
                               PirFunction *fn, PirBlockId b, uint32_t j,
                               PirInstIdVec *head, const PirInst *inst) {
    PirType tptr  = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti8   = pir_type_scalar(PIR_SCALAR_I8);
    PirType ti32  = pir_type_scalar(PIR_SCALAR_I32);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);
    uint32_t ln = inst->src_line, lc = inst->loc_idx;

    bool is_buf = (inst->op == PIR_OP_VA_ARG_BUF);
    PirOperand ap = is_buf ? inst->operands[1] : inst->operands[0];
    PirType arg_ty = is_buf ? inst->aux_type : inst->ty;

    PirVaArgPlan plan;
    pir_abi_plan_va_arg(mod, target, arg_ty, is_buf, &plan);

    if (plan.is_empty) {
        PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, NULL);
        PirInst jc = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jc.targets[0].target = cont_blk;
        pir_fn_append_inst(fn, &fn->blocks.data[b].insts, jc);
        return;
    }

    PirBlockId reg_blk = pir_fn_new_block(mod, fn, "va_reg", ln);
    PirBlockId mem_blk = pir_fn_new_block(mod, fn, "va_mem", ln);
    PirValueId arg_ptr_param = PIR_INVALID_ID;
    PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, &arg_ptr_param);

    bool use_fpr  = plan.is_fp;
    bool need_2gp = (!use_fpr && !plan.is_indirect && plan.size > 4u); /* i64 -> 2 GP regs aligned even */
    uint64_t idx_off = use_fpr ? 1u : 0u;
    uint32_t reg_cnt = need_2gp ? 2u : 1u;
    uint32_t reg_lim = 8u - reg_cnt; /* valid if aligned_idx <= reg_lim */

    PirInstIdVec *cur = &fn->blocks.data[b].insts;
    PirValueId idx_p = va_padd(mod, fn, b, cur, ap, idx_off, ti32, ln, lc);
    PirInst li8 = pir_make_inst(PIR_OP_LOAD, ti8, ln, lc);
    li8.align = 1u; li8.operand_count = 1u; li8.operands = pir_alloc_ops(mod, 1u);
    li8.operands[0] = pir_op_val(idx_p, tptr);
    li8.results[0]  = pir_fn_new_val(mod, fn, "vaidx8", ti8, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, cur, li8);

    PirInst zx = pir_make_inst(PIR_OP_ZEXT, ti32, ln, lc);
    zx.operand_count = 1u; zx.operands = pir_alloc_ops(mod, 1u);
    zx.operands[0] = pir_op_val(li8.results[0], ti8);
    zx.results[0]  = pir_fn_new_val(mod, fn, "vaidx", ti32, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, cur, zx);

    PirValueId cur_idx = zx.results[0];
    if (need_2gp) {
        /* Align gpr index to even: (idx + 1) & ~1 */
        PirInst a1 = pir_make_inst(PIR_OP_ADD, ti32, ln, lc);
        a1.operand_count = 2u; a1.operands = pir_alloc_ops(mod, 2u);
        a1.operands[0] = pir_op_val(cur_idx, ti32);
        a1.operands[1] = pir_op_imm_i(1u, false, ti32);
        a1.results[0]  = pir_fn_new_val(mod, fn, "vaa1", ti32, b, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, cur, a1);

        PirInst m2 = pir_make_inst(PIR_OP_AND, ti32, ln, lc);
        m2.operand_count = 2u; m2.operands = pir_alloc_ops(mod, 2u);
        m2.operands[0] = pir_op_val(a1.results[0], ti32);
        m2.operands[1] = pir_op_imm_i(UINT64_C(0xFFFFFFFE), false, ti32);
        m2.results[0]  = pir_fn_new_val(mod, fn, "vaai", ti32, b, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, cur, m2);
        cur_idx = m2.results[0];
    }

    PirInst cmp = pir_make_inst(PIR_OP_CMP_LE_U, ti32, ln, lc);
    cmp.operand_count = 2u; cmp.operands = pir_alloc_ops(mod, 2u);
    cmp.operands[0] = pir_op_val(cur_idx, ti32);
    cmp.operands[1] = pir_op_imm_i(reg_lim, false, ti32);
    cmp.results[0]  = pir_fn_new_val(mod, fn, "vainr", ti8, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, cur, cmp);

    PirInst br = pir_make_inst(PIR_OP_BR, tvoid, ln, lc);
    br.operand_count = 1u; br.operands = pir_alloc_ops(mod, 1u);
    br.operands[0] = pir_op_val(cmp.results[0], ti8);
    br.targets[0].target = reg_blk;
    br.targets[1].target = mem_blk;
    pir_fn_append_inst(fn, cur, br);

    /* reg_blk: ptr = reg_save_area + (use_fpr ? 32 + idx*8 : idx*4); idx += reg_cnt */
    PirInstIdVec *rb = &fn->blocks.data[reg_blk].insts;
    PirValueId rsa_p = va_padd(mod, fn, reg_blk, rb, ap, 8u, ti32, ln, lc);
    PirInst lrsa = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
    lrsa.align = 4u; lrsa.operand_count = 1u; lrsa.operands = pir_alloc_ops(mod, 1u);
    lrsa.operands[0] = pir_op_val(rsa_p, tptr);
    lrsa.results[0]  = pir_fn_new_val(mod, fn, "varsa", tptr, reg_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, rb, lrsa);

    PirInst sh = pir_make_inst(PIR_OP_SHL, ti32, ln, lc);
    sh.flags = PIR_FLAG_MASK; sh.operand_count = 2u; sh.operands = pir_alloc_ops(mod, 2u);
    sh.operands[0] = pir_op_val(cur_idx, ti32);
    sh.operands[1] = pir_op_imm_i(use_fpr ? 3u : 2u, false, ti32);
    sh.results[0]  = pir_fn_new_val(mod, fn, "vaboff", ti32, reg_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, rb, sh);

    PirValueId boff = sh.results[0];
    if (use_fpr) {
        PirInst a32 = pir_make_inst(PIR_OP_ADD, ti32, ln, lc);
        a32.operand_count = 2u; a32.operands = pir_alloc_ops(mod, 2u);
        a32.operands[0] = pir_op_val(boff, ti32);
        a32.operands[1] = pir_op_imm_i(32u, false, ti32);
        a32.results[0]  = pir_fn_new_val(mod, fn, "vafbo", ti32, reg_blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, rb, a32);
        boff = a32.results[0];
    }

    PirInst rslot = pir_make_inst(PIR_OP_PTR_ADD, ti32, ln, lc);
    rslot.operand_count = 2u; rslot.operands = pir_alloc_ops(mod, 2u);
    rslot.operands[0] = pir_op_val(lrsa.results[0], tptr);
    rslot.operands[1] = pir_op_val(boff, ti32);
    rslot.results[0]  = pir_fn_new_val(mod, fn, "varsl", tptr, reg_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, rb, rslot);

    PirInst ni = pir_make_inst(PIR_OP_ADD, ti32, ln, lc);
    ni.operand_count = 2u; ni.operands = pir_alloc_ops(mod, 2u);
    ni.operands[0] = pir_op_val(cur_idx, ti32);
    ni.operands[1] = pir_op_imm_i(reg_cnt, false, ti32);
    ni.results[0]  = pir_fn_new_val(mod, fn, "vani", ti32, reg_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, rb, ni);

    PirInst ni8 = pir_make_inst(PIR_OP_TRUNC, ti8, ln, lc);
    ni8.operand_count = 1u; ni8.operands = pir_alloc_ops(mod, 1u);
    ni8.operands[0] = pir_op_val(ni.results[0], ti32);
    ni8.results[0]  = pir_fn_new_val(mod, fn, "vani8", ti8, reg_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, rb, ni8);

    PirInst si8 = pir_make_inst(PIR_OP_STORE, ti8, ln, lc);
    si8.align = 1u; si8.operand_count = 2u; si8.operands = pir_alloc_ops(mod, 2u);
    si8.operands[0] = pir_op_val(ni8.results[0], ti8);
    si8.operands[1] = pir_op_val(idx_p, tptr);
    pir_fn_append_inst(fn, rb, si8);

    PirInst jr = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
    jr.targets[0].target    = cont_blk;
    jr.targets[0].arg_count = 1u;
    jr.targets[0].args      = pir_alloc_ops(mod, 1u);
    jr.targets[0].args[0]   = pir_op_val(rslot.results[0], tptr);
    pir_fn_append_inst(fn, rb, jr);

    /* mem_blk: mark gpr/fpr exhausted (8), consume from overflow_arg_area (@4) */
    PirInstIdVec *mb = &fn->blocks.data[mem_blk].insts;
    PirInst s8 = pir_make_inst(PIR_OP_STORE, ti8, ln, lc);
    s8.align = 1u; s8.operand_count = 2u; s8.operands = pir_alloc_ops(mod, 2u);
    s8.operands[0] = pir_op_imm_i(8u, false, ti8);
    s8.operands[1] = pir_op_val(idx_p, tptr);
    pir_fn_append_inst(fn, mb, s8);

    PirValueId oaa_p = va_padd(mod, fn, mem_blk, mb, ap, 4u, ti32, ln, lc);
    PirInst loaa = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
    loaa.align = 4u; loaa.operand_count = 1u; loaa.operands = pir_alloc_ops(mod, 1u);
    loaa.operands[0] = pir_op_val(oaa_p, tptr);
    loaa.results[0]  = pir_fn_new_val(mod, fn, "vaoaa", tptr, mem_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, mb, loaa);

    PirValueId mslot = loaa.results[0];
    if (plan.mem_align > 4u) {
        mslot = pir_emit_align_ptr(mod, fn, mem_blk, mb, mslot, plan.mem_align, target, ln, lc);
    }

    PirInst noaa = pir_make_inst(PIR_OP_PTR_ADD, ti32, ln, lc);
    noaa.operand_count = 2u; noaa.operands = pir_alloc_ops(mod, 2u);
    noaa.operands[0] = pir_op_val(mslot, tptr);
    noaa.operands[1] = pir_op_imm_i(plan.mem_step, false, ti32);
    noaa.results[0]  = pir_fn_new_val(mod, fn, "vanoaa", tptr, mem_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, mb, noaa);

    PirInst soaa = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
    soaa.align = 4u; soaa.operand_count = 2u; soaa.operands = pir_alloc_ops(mod, 2u);
    soaa.operands[0] = pir_op_val(noaa.results[0], tptr);
    soaa.operands[1] = pir_op_val(oaa_p, tptr);
    pir_fn_append_inst(fn, mb, soaa);

    PirInst jm = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
    jm.targets[0].target    = cont_blk;
    jm.targets[0].arg_count = 1u;
    jm.targets[0].args      = pir_alloc_ops(mod, 1u);
    jm.targets[0].args[0]   = pir_op_val(mslot, tptr);
    pir_fn_append_inst(fn, mb, jm);

    /* Prepend materialization in cont_blk. */
    PirInstIdVec mat = {0};
    emit_va_arg_materialize(mod, target, fn, cont_blk, &mat, inst, &plan, arg_ptr_param);
    PirBlock *cb = &fn->blocks.data[cont_blk];
    for (uint32_t k = 0u; k < cb->insts.len; ++k) {
        PIR_VEC_PUSH(&mat, cb->insts.data[k]);
    }
    PIR_VEC_FREE(&cb->insts);
    cb->insts = mat;
}

bool pir_lower_varargs(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    (void)diag;
    bool is_sysv_x64     = (target->arch == PIR_ARCH_X86_64 && target->os != PIR_OS_WINDOWS);
    bool is_aapcs64_lin  = (target->arch == PIR_ARCH_ARM64  && target->os != PIR_OS_MACOS);
    bool is_sysv_ppc32   = (target->arch == PIR_ARCH_PPC32);
    bool is_ptr_cursor   = !is_sysv_x64 && !is_aapcs64_lin && !is_sysv_ppc32; /* Darwin ARM64, Win64, RV32 */
    PirType tptr   = pir_type_scalar(PIR_SCALAR_PTR);
    PirType idx_ty = pir_type_scalar(target->ptr_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);
    PirType tvoid  = pir_type_scalar(PIR_SCALAR_VOID);
    uint32_t ptr_al = target->ptr_bytes > 0u ? (uint32_t)target->ptr_bytes : 8u;

    /* 1. Rewrite byval(va_list) on call / call_indirect sites (Darwin/Win64 load the 8B char* cursor)
     *    and on function/sig declarations BEFORE cabi runs. */
    bool *fn_has_valist_byval  = (bool *)calloc(mod->funcs.len + 1u, sizeof(bool));
    bool *sig_has_valist_byval = (bool *)calloc(mod->sigs.len + 1u, sizeof(bool));
    if (!fn_has_valist_byval || !sig_has_valist_byval) abort();
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        const PirFunction *f = &mod->funcs.data[i];
        for (uint32_t k = 0u; k < f->param_count; ++k) {
            if (f->params[k].abi_kind == PIR_PARAM_BYVAL && is_valist_type(f->params[k].abi_type)) {
                fn_has_valist_byval[i] = true;
                break;
            }
        }
    }
    for (uint32_t i = 0u; i < mod->sigs.len; ++i) {
        const PirSigDef *s = &mod->sigs.data[i];
        for (uint32_t k = 0u; k < s->param_count; ++k) {
            if (s->params[k].abi_kind == PIR_PARAM_BYVAL && is_valist_type(s->params[k].abi_type)) {
                sig_has_valist_byval[i] = true;
                break;
            }
        }
    }

    /* Rewrite byval(va_list) at call / call_indirect sites across ALL targets
     * (fixed params and variadic args alike) before cabi runs. */
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        PirFunction *fn = &mod->funcs.data[fi];
        if (!fn->is_defined) continue;
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            PirInstIdVec out = {0};
            for (uint32_t j = 0u; j < fn->blocks.data[b].insts.len; ++j) {
                PirInstId iid = fn->blocks.data[b].insts.data[j];
                PirInst copy  = fn->insts.data[iid];
                const PirParamDecl *cal_params = NULL;
                uint32_t cal_pcount = 0u;
                bool has_any = false;
                if (copy.op == PIR_OP_CALL && copy.operands[0].kind == PIR_OPER_SYM &&
                    copy.operands[0].u.sym.is_func) {
                    const PirFunction *cal = &mod->funcs.data[copy.operands[0].u.sym.id];
                    cal_params = cal->params;
                    cal_pcount = cal->param_count;
                    if (fn_has_valist_byval[copy.operands[0].u.sym.id]) has_any = true;
                } else if (copy.op == PIR_OP_CALL_INDIRECT && copy.sig_id < mod->sigs.len) {
                    const PirSigDef *sig = &mod->sigs.data[copy.sig_id];
                    cal_params = sig->params;
                    cal_pcount = sig->param_count;
                    if (sig_has_valist_byval[copy.sig_id]) has_any = true;
                }
                if ((copy.op == PIR_OP_CALL || copy.op == PIR_OP_CALL_INDIRECT) && !has_any) {
                    for (uint32_t k = 1u; k < copy.operand_count; ++k) {
                        if (copy.operands[k].call_abi == PIR_PARAM_BYVAL &&
                            is_valist_type(copy.operands[k].call_abi_type)) {
                            has_any = true;
                            break;
                        }
                    }
                }
                if (has_any) {
                    PirOperand *nops = pir_alloc_ops(mod, copy.operand_count);
                    memcpy(nops, copy.operands, copy.operand_count * sizeof(PirOperand));
                    for (uint32_t k = 1u; k < copy.operand_count; ++k) {
                        uint32_t pi = k - 1u;
                        bool is_fixed_vl = (cal_params && pi < cal_pcount &&
                                            cal_params[pi].abi_kind == PIR_PARAM_BYVAL &&
                                            is_valist_type(cal_params[pi].abi_type));
                        bool is_call_vl  = (nops[k].call_abi == PIR_PARAM_BYVAL &&
                                            is_valist_type(nops[k].call_abi_type));
                        if (!is_fixed_vl && !is_call_vl) continue;

                        if (is_ptr_cursor) {
                            PirInst ld = pir_make_inst(PIR_OP_LOAD, tptr, copy.src_line, copy.loc_idx);
                            ld.align = ptr_al; ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
                            ld.operands[0] = nops[k];
                            pir_op_strip_call_meta(&ld.operands[0]);
                            ld.results[0]  = pir_fn_new_val(mod, fn, "vap_v", tptr, b, PIR_INVALID_ID, 0u);
                            pir_fn_append_inst(fn, &out, ld);
                            nops[k] = pir_op_val(ld.results[0], tptr);
                        } else if (is_aapcs64_lin) {
                            if (nops[k].call_abi == PIR_PARAM_BYVAL) {
                                nops[k].call_abi_type = pir_type_agg(ensure_aapcs64_valist_struct(mod));
                            }
                        } else {
                            /* SysV x64: va_list decays to plain ptr. */
                            nops[k].call_abi          = PIR_PARAM_NORMAL;
                            nops[k].call_abi_type     = tvoid;
                            nops[k].type              = tptr;
                            nops[k].has_explicit_type = false;
                        }
                    }
                    fn->insts.data[iid].operands = nops;
                }
                PIR_VEC_PUSH(&out, iid);
            }
            PIR_VEC_FREE(&fn->blocks.data[b].insts);
            fn->blocks.data[b].insts = out;
        }
    }
    free(fn_has_valist_byval);
    free(sig_has_valist_byval);

    /* Strip or transform byval(va_list) in function & signature parameter declarations. */
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        PirFunction *f = &mod->funcs.data[i];
        PirInstIdVec prologue = {0};
        for (uint32_t k = 0u; k < f->param_count; ++k) {
            if (f->params[k].abi_kind == PIR_PARAM_BYVAL && is_valist_type(f->params[k].abi_type)) {
                if (is_aapcs64_lin) {
                    f->params[k].abi_type = pir_type_agg(ensure_aapcs64_valist_struct(mod));
                } else {
                    /* SysV x64 decays va_list[1] -> ptr; Darwin/Win64 passes char* cursor value,
                     * so a defined callee must spill the incoming cursor into a local 8B slot
                     * reusing the original param ValueId (which body uses as char** via va_arg). */
                    f->params[k].abi_kind = PIR_PARAM_NORMAL;
                    f->params[k].abi_type = tvoid;
                    f->params[k].type     = tptr;
                    if (is_ptr_cursor && f->is_defined && f->blocks.len > 0u &&
                        f->params[k].val_id != PIR_INVALID_ID) {
                        PirValueId orig_v = f->params[k].val_id;
                        PirValueId raw_p = pir_fn_new_val(mod, f, "va_in", tptr,
                                                          PIR_INVALID_ID, PIR_INVALID_ID, k);
                        f->params[k].val_id = raw_p;
                        f->params[k].name   = f->values.data[raw_p].name;

                        f->values.data[orig_v].def_block   = 0u;
                        f->values.data[orig_v].param_index = 0u;
                        PirInst al_i = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, f->src_line, 0u);
                        al_i.imm_u64    = target->va_list_size;
                        al_i.align      = target->va_list_align;
                        al_i.results[0] = orig_v;
                        pir_fn_append_inst(f, &prologue, al_i);

                        PirInst st_i = pir_make_inst(PIR_OP_STORE, tptr, f->src_line, 0u);
                        st_i.align = ptr_al; st_i.operand_count = 2u; st_i.operands = pir_alloc_ops(mod, 2u);
                        st_i.operands[0] = pir_op_val(raw_p, tptr);
                        st_i.operands[1] = pir_op_val(orig_v, tptr);
                        pir_fn_append_inst(f, &prologue, st_i);
                    }
                }
            }
        }
        pir_fn_prepend_entry_insts(f, &prologue);
    }
    for (uint32_t i = 0u; i < mod->sigs.len; ++i) {
        PirSigDef *s = &mod->sigs.data[i];
        for (uint32_t k = 0u; k < s->param_count; ++k) {
            if (s->params[k].abi_kind == PIR_PARAM_BYVAL && is_valist_type(s->params[k].abi_type)) {
                if (is_aapcs64_lin) {
                    s->params[k].abi_type = pir_type_agg(ensure_aapcs64_valist_struct(mod));
                } else {
                    s->params[k].abi_kind = PIR_PARAM_NORMAL;
                    s->params[k].abi_type = tvoid;
                    s->params[k].type     = tptr;
                }
            }
        }
    }

    /* 1b. Expand oversized vector va_arg (> max_simd_bits) into piece-wise va_args + stack reassembly
     *     matching pir_legalize_simd's call-site argument splitting, EXCEPT when the target ABI
     *     passes the vector as a single indirect pointer or MEMORY slot (Win64 >8B, ARM64/SysV >16B). */
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        PirFunction *fn = &mod->funcs.data[fi];
        if (!fn->is_defined) continue;
        PirInstIdVec entry_allocas = {0};
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            bool has_ov = false;
            for (uint32_t j = 0u; j < fn->blocks.data[b].insts.len; ++j) {
                const PirInst *ins = &fn->insts.data[fn->blocks.data[b].insts.data[j]];
                if (ins->op == PIR_OP_VA_ARG && ins->ty.kind == PIR_TYPE_VECTOR) {
                    PirVaArgPlan vp;
                    pir_abi_plan_va_arg(mod, target, ins->ty, false, &vp);
                    if (vp.split_vec) {
                        has_ov = true;
                        break;
                    }
                }
            }
            if (!has_ov) continue;
            PirInstIdVec out = {0};
            for (uint32_t j = 0u; j < fn->blocks.data[b].insts.len; ++j) {
                PirInstId iid = fn->blocks.data[b].insts.data[j];
                PirInst copy  = fn->insts.data[iid];
                bool do_split = false;
                if (copy.op == PIR_OP_VA_ARG && copy.ty.kind == PIR_TYPE_VECTOR) {
                    PirVaArgPlan vp;
                    pir_abi_plan_va_arg(mod, target, copy.ty, false, &vp);
                    do_split = vp.split_vec;
                }
                if (do_split) {
                    PirType pty;
                    uint32_t cnt = 0u;
                    va_split_vec_spec(copy.ty.u.vec, target->caps.max_simd_bits, &pty, &cnt);
                    PirTypeLayout vly = pir_type_layout(mod, target, copy.ty);
                    PirTypeLayout ply = pir_type_layout(mod, target, pty);
                    uint32_t ln = copy.src_line, lc = copy.loc_idx;

                    PirValueId vav_v = pir_fn_emit_entry_alloca(mod, fn, "vav",
                                                                vly.size, vly.align,
                                                                ln, lc, &entry_allocas);

                    for (uint32_t p = 0u; p < cnt; ++p) {
                        PirInst va = pir_make_inst(PIR_OP_VA_ARG, pty, ln, lc);
                        va.operand_count = 1u; va.operands = pir_alloc_ops(mod, 1u);
                        va.operands[0] = copy.operands[0];
                        va.results[0]  = pir_fn_new_val(mod, fn, "vav_p", pty, b, PIR_INVALID_ID, 0u);
                        pir_fn_append_inst(fn, &out, va);

                        PirValueId sp = va_padd(mod, fn, b, &out, pir_op_val(vav_v, tptr),
                                                (uint64_t)p * ply.size, idx_ty, ln, lc);
                        PirInst st = pir_make_inst(PIR_OP_STORE, pty, ln, lc);
                        st.align = ply.align; st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
                        st.operands[0] = pir_op_val(va.results[0], pty);
                        st.operands[1] = pir_op_val(sp, tptr);
                        pir_fn_append_inst(fn, &out, st);
                    }
                    PirInst ld = pir_make_inst(PIR_OP_LOAD, copy.ty, ln, lc);
                    ld.align = vly.align; ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
                    ld.operands[0] = pir_op_val(vav_v, tptr);
                    ld.results[0]  = copy.results[0];
                    pir_fn_append_inst(fn, &out, ld);
                } else {
                    PIR_VEC_PUSH(&out, iid);
                }
            }
            PIR_VEC_FREE(&fn->blocks.data[b].insts);
            fn->blocks.data[b].insts = out;
        }
        pir_fn_prepend_entry_insts(fn, &entry_allocas);
    }

    /* 2. Lower alloca va_list, va_copy, va_end, va_arg, va_arg_buf inside every function. */
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        PirFunction *fn = &mod->funcs.data[fi];
        if (!fn->is_defined) continue;

        /* Loop over blocks; note block splitting appends cont_blk which we continue scanning! */
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            PirInstIdVec out = {0};
            bool split = false;
            for (uint32_t j = 0u; j < fn->blocks.data[b].insts.len; ++j) {
                PirInstId iid = fn->blocks.data[b].insts.data[j];
                PirInst copy  = fn->insts.data[iid];

                if (copy.op == PIR_OP_ALLOCA && is_valist_type(copy.aux_type)) {
                    PirInst *slot = &fn->insts.data[iid];
                    slot->op       = PIR_OP_ALLOCA_PTR;
                    slot->ty       = tptr;
                    slot->aux_type = tvoid;
                    slot->imm_u64  = target->va_list_size;
                    slot->align    = target->va_list_align;
                    PIR_VEC_PUSH(&out, iid);
                } else if (copy.op == PIR_OP_VA_COPY) {
                    PirInst mc = pir_make_inst(PIR_OP_MEMCPY, tvoid, copy.src_line, copy.loc_idx);
                    mc.align = target->va_list_align;
                    mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
                    mc.operands[0] = copy.operands[0];
                    mc.operands[1] = copy.operands[1];
                    mc.operands[2] = pir_op_imm_i(target->va_list_size, false, idx_ty);
                    pir_fn_append_inst(fn, &out, mc);
                } else if (copy.op == PIR_OP_VA_END) {
                    /* Delete va_end. */
                } else if (copy.op == PIR_OP_VA_ARG || copy.op == PIR_OP_VA_ARG_BUF) {
                    if (is_ptr_cursor) {
                        lower_va_arg_ptr_cursor(mod, target, fn, b, &out, &copy);
                    } else if (is_sysv_x64) {
                        lower_va_arg_sysv(mod, target, fn, b, j, &out, &copy);
                        split = true;
                        break;
                    } else if (is_sysv_ppc32) {
                        lower_va_arg_ppc32(mod, target, fn, b, j, &out, &copy);
                        split = true;
                        break;
                    } else {
                        lower_va_arg_aapcs64_linux(mod, target, fn, b, j, &out, &copy);
                        split = true;
                        break;
                    }
                } else {
                    PIR_VEC_PUSH(&out, iid);
                }
            }
            if (!split) {
                PIR_VEC_FREE(&fn->blocks.data[b].insts);
                fn->blocks.data[b].insts = out;
            }
        }
    }
    return true;
}
