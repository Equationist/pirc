/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lower/lower.h"
#include "target/abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t va_align_up(uint64_t v, uint64_t al) {
    return (v + al - 1u) & ~(al - 1u);
}

static bool va_agg_is_indirect(const PirModule *mod, const PirTargetSpec *target, PirType arg_ty) {
    PirParamDecl pd;
    memset(&pd, 0, sizeof(pd));
    pd.abi_kind = PIR_PARAM_BYVAL;
    pd.abi_type = arg_ty;
    pd.type     = pir_type_scalar(PIR_SCALAR_PTR);
    PirAbiRegState st = { 0u, 0u };
    PirAbiArgInfo info;
    if (!pir_abi_classify_param(mod, target, &pd, &st, &info)) {
        return false;
    }
    return info.kind == PIR_ABI_INDIRECT && info.stack_byval_size == 0u;
}

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

static PirBlockId lower_va_arg_sysv_agg(PirModule *mod, const PirTargetSpec *target,
                                        PirFunction *fn, PirBlockId b, uint32_t j,
                                        PirInstIdVec *head, const PirInst *inst) {
    PirType tptr  = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti32  = pir_type_scalar(PIR_SCALAR_I32);
    PirType ti64  = pir_type_scalar(PIR_SCALAR_I64);
    PirType ti8   = pir_type_scalar(PIR_SCALAR_I8);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);
    uint32_t ln = inst->src_line, lc = inst->loc_idx;

    PirType arg_ty     = inst->aux_type;
    PirOperand dst_buf = inst->operands[0];
    PirOperand ap      = inst->operands[1];
    PirTypeLayout ly   = pir_type_layout(mod, target, arg_ty);
    uint64_t mem_step  = va_align_up(ly.size > 0u ? ly.size : 8u, 8u);

    PirParamDecl pd;
    memset(&pd, 0, sizeof(pd));
    pd.abi_kind = PIR_PARAM_BYVAL;
    pd.abi_type = arg_ty;
    pd.type     = tptr;
    PirAbiRegState st0 = { 0u, 0u };
    PirAbiArgInfo info;
    bool classified = pir_abi_classify_param(mod, target, &pd, &st0, &info);
    bool is_empty   = (ly.size == 0u) ||
                      (classified && info.kind == PIR_ABI_DIRECT && info.reg_count == 0u);
    if (is_empty) {
        PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, NULL);
        PirInst jc = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jc.targets[0].target = cont_blk;
        pir_fn_append_inst(fn, &fn->blocks.data[b].insts, jc);
        return cont_blk;
    }
    bool use_regs   = classified && info.kind == PIR_ABI_DIRECT &&
                      info.reg_count > 0u && (info.gp_used > 0u || info.fp_used > 0u);

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
            uint64_t rem     = (ly.size > dst_off) ? (ly.size - dst_off) : 0u;
            uint64_t chunk   = (rty.kind == PIR_TYPE_VECTOR && pir_vec_bits(rty.u.vec) == 128u)
                               ? (rem < 16u ? rem : 16u)
                               : (rem < 8u ? rem : 8u);
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

    /* mem_blk: load overflow_arg_area (@8), align up if align > 8, copy to dst_buf, advance */
    PirInstIdVec *mb = &fn->blocks.data[mem_blk].insts;
    PirValueId oaa_p = va_padd(mod, fn, mem_blk, mb, ap, 8u, ti64, ln, lc);
    PirInst loaa = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
    loaa.align = 8u; loaa.operand_count = 1u; loaa.operands = pir_alloc_ops(mod, 1u);
    loaa.operands[0] = pir_op_val(oaa_p, tptr);
    loaa.results[0]  = pir_fn_new_val(mod, fn, "vaoaa", tptr, mem_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, mb, loaa);

    PirValueId cur_oaa = loaa.results[0];
    if (ly.align > 8u) {
        uint64_t al = (ly.align > 16u) ? ly.align : 16u;
        cur_oaa = pir_emit_align_ptr(mod, fn, mem_blk, mb, cur_oaa, al, target, ln, lc);
    }

    if (ly.size > 0u) {
        PirInst mc = pir_make_inst(PIR_OP_MEMCPY, tvoid, ln, lc);
        mc.align = ly.align < 8u ? ly.align : 8u;
        mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
        mc.operands[0] = dst_buf;
        mc.operands[1] = pir_op_val(cur_oaa, tptr);
        mc.operands[2] = pir_op_imm_i(ly.size, false, ti64);
        pir_fn_append_inst(fn, mb, mc);
    }

    PirInst noaa = pir_make_inst(PIR_OP_PTR_ADD, ti64, ln, lc);
    noaa.operand_count = 2u; noaa.operands = pir_alloc_ops(mod, 2u);
    noaa.operands[0] = pir_op_val(cur_oaa, tptr);
    noaa.operands[1] = pir_op_imm_i(mem_step, false, ti64);
    noaa.results[0]  = pir_fn_new_val(mod, fn, "vanoaa", tptr, mem_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, mb, noaa);

    PirInst soaa = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
    soaa.align = 8u; soaa.operand_count = 2u; soaa.operands = pir_alloc_ops(mod, 2u);
    soaa.operands[0] = pir_op_val(noaa.results[0], tptr);
    soaa.operands[1] = pir_op_val(oaa_p, tptr);
    pir_fn_append_inst(fn, mb, soaa);

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

    PirType arg_ty = inst->ty;
    bool promote_f32 = (arg_ty.kind == PIR_TYPE_SCALAR && arg_ty.u.scalar == PIR_SCALAR_F32);
    PirType load_ty = promote_f32 ? pir_type_scalar(PIR_SCALAR_F64) : arg_ty;
    PirOperand ap  = inst->operands[0];
    PirTypeLayout ly = pir_type_layout(mod, target, load_ty);
    uint64_t step = va_align_up(ly.size > 0u ? ly.size : 8u, 8u);
    bool is_fp     = (pir_type_is_float(load_ty) || load_ty.kind == PIR_TYPE_VECTOR);
    bool force_mem = (load_ty.kind == PIR_TYPE_VECTOR && ly.size > 16u);

    PirBlockId reg_blk = force_mem ? PIR_INVALID_ID : pir_fn_new_block(mod, fn, "va_reg", ln);
    PirBlockId mem_blk = pir_fn_new_block(mod, fn, "va_mem", ln);
    PirValueId res_ptr = PIR_INVALID_ID;
    PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, &res_ptr);

    /* SysV va_list: { gp_offset: i32 @0, fp_offset: i32 @4, overflow_arg_area: ptr @8, reg_save_area: ptr @16 } */
    PirInstIdVec *cur = &fn->blocks.data[b].insts;
    if (force_mem) {
        /* SysV AMD64 §3.5.7: 256-bit vectors are always passed in MEMORY (overflow_arg_area),
         * never in the 176-byte reg_save_area (which only holds 128-bit xmm0..xmm7). */
        PirInst jm0 = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jm0.targets[0].target = mem_blk;
        pir_fn_append_inst(fn, cur, jm0);
    } else {
        uint64_t off_field = is_fp ? 4u : 0u;
        uint64_t limit     = is_fp ? 176u : 48u;
        uint64_t reg_step  = is_fp ? 16u : (step > 8u ? step : 8u);

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
    PirValueId oaa_p = va_padd(mod, fn, mem_blk, mb, ap, 8u, ti64, ln, lc);
    PirInst loaa = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
    loaa.align = 8u; loaa.operand_count = 1u; loaa.operands = pir_alloc_ops(mod, 1u);
    loaa.operands[0] = pir_op_val(oaa_p, tptr);
    loaa.results[0]  = pir_fn_new_val(mod, fn, "vaoaa", tptr, mem_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, mb, loaa);

    PirValueId arg_mem = loaa.results[0];
    if (ly.align > 8u) {
        arg_mem = pir_emit_align_ptr(mod, fn, mem_blk, mb, arg_mem, ly.align, target, ln, lc);
    }

    PirInst noaa = pir_make_inst(PIR_OP_PTR_ADD, ti64, ln, lc);
    noaa.operand_count = 2u; noaa.operands = pir_alloc_ops(mod, 2u);
    noaa.operands[0] = pir_op_val(arg_mem, tptr);
    noaa.operands[1] = pir_op_imm_i(step, false, ti64);
    noaa.results[0]  = pir_fn_new_val(mod, fn, "vanoaa", tptr, mem_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, mb, noaa);

    PirInst soaa = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
    soaa.align = 8u; soaa.operand_count = 2u; soaa.operands = pir_alloc_ops(mod, 2u);
    soaa.operands[0] = pir_op_val(noaa.results[0], tptr);
    soaa.operands[1] = pir_op_val(oaa_p, tptr);
    pir_fn_append_inst(fn, mb, soaa);

    PirInst jm = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
    jm.targets[0].target = cont_blk; jm.targets[0].arg_count = 1u;
    jm.targets[0].args = pir_alloc_ops(mod, 1u);
    jm.targets[0].args[0] = pir_op_val(arg_mem, tptr);
    pir_fn_append_inst(fn, mb, jm);

    /* Prepend load to cont_blk. */
    PirInstIdVec prep = {0};
    if (promote_f32) {
        PirInst ld = pir_make_inst(PIR_OP_LOAD, load_ty, ln, lc);
        ld.align = 8u;
        ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
        ld.operands[0] = pir_op_val(res_ptr, tptr);
        ld.results[0]  = pir_fn_new_val(mod, fn, "vaf64", load_ty, cont_blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, &prep, ld);

        PirInst ft = pir_make_inst(PIR_OP_FPTRUNC, arg_ty, ln, lc);
        ft.operand_count = 1u; ft.operands = pir_alloc_ops(mod, 1u);
        ft.operands[0] = pir_op_val(ld.results[0], load_ty);
        ft.results[0]  = inst->results[0];
        fn->values.data[inst->results[0]].def_block = cont_blk;
        pir_fn_append_inst(fn, &prep, ft);
    } else {
        PirInst ld = pir_make_inst(PIR_OP_LOAD, arg_ty, ln, lc);
        ld.align = ly.align < 8u ? ly.align : 8u;
        ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
        ld.operands[0] = pir_op_val(res_ptr, tptr);
        ld.results[0]  = inst->results[0];
        fn->values.data[inst->results[0]].def_block = cont_blk;
        pir_fn_append_inst(fn, &prep, ld);
    }
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
    PirType arg_ty = is_buf ? inst->aux_type : inst->ty;
    bool promote_f32 = (!is_buf && arg_ty.kind == PIR_TYPE_SCALAR && arg_ty.u.scalar == PIR_SCALAR_F32);
    PirType load_ty = promote_f32 ? pir_type_scalar(PIR_SCALAR_F64) : arg_ty;
    PirOperand ap  = is_buf ? inst->operands[1] : inst->operands[0];
    PirTypeLayout ly = pir_type_layout(mod, target, load_ty);
    uint32_t nat_al  = pir_abi_natural_align(mod, target, load_ty);
    if (is_buf && (ly.size == 0u || pir_abi_is_pad_only_agg(mod, arg_ty))) {
        PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, NULL);
        PirInst jc = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
        jc.targets[0].target = cont_blk;
        pir_fn_append_inst(fn, &fn->blocks.data[b].insts, jc);
        return cont_blk;
    }
    bool indirect_val = (!is_buf && load_ty.kind == PIR_TYPE_VECTOR && ly.size > 16u);
    bool indirect = (is_buf && va_agg_is_indirect(mod, target, arg_ty)) || indirect_val;
    uint64_t step = indirect ? 8u : va_align_up(ly.size > 0u ? ly.size : 8u, 8u);

    /* Classify aggregate for HFA (uses FP save area __vr_top / __vr_offs). */
    bool is_hfa = false;
    PirAbiArgInfo hfa_info;
    memset(&hfa_info, 0, sizeof(hfa_info));
    if (is_buf && !indirect) {
        PirParamDecl pd;
        memset(&pd, 0, sizeof(pd));
        pd.abi_kind = PIR_PARAM_BYVAL;
        pd.abi_type = arg_ty;
        pd.type     = tptr;
        PirAbiRegState st0 = { 0u, 0u };
        if (pir_abi_classify_param(mod, target, &pd, &st0, &hfa_info) &&
            hfa_info.kind == PIR_ABI_DIRECT && hfa_info.fp_used > 0u) {
            is_hfa = true;
        }
    }
    bool is_fp = is_hfa || (!is_buf && !indirect_val && (pir_type_is_float(load_ty) || load_ty.kind == PIR_TYPE_VECTOR));

    PirBlockId reg_blk = pir_fn_new_block(mod, fn, "va_reg", ln);
    PirBlockId mem_blk = pir_fn_new_block(mod, fn, "va_mem", ln);
    PirValueId res_ptr = PIR_INVALID_ID;
    PirBlockId cont_blk = split_block_tail(mod, fn, b, j, head, ln, is_hfa ? NULL : &res_ptr);

    /* Linux AAPCS64 va_list: { __stack: ptr @0, __gr_top: ptr @8, __vr_top: ptr @16, __gr_offs: i32 @24, __vr_offs: i32 @28 } */
    uint64_t top_off  = is_fp ? 16u : 8u;
    uint64_t offs_off = is_fp ? 28u : 24u;
    uint64_t reg_step = is_hfa ? ((uint64_t)hfa_info.fp_used * 16u)
                      : is_fp  ? 16u
                               : (step > 8u ? step : 8u);
    bool align_gp16 = !is_fp && !indirect && nat_al >= 16u;

    PirInstIdVec *cur = &fn->blocks.data[b].insts;
    PirValueId op = va_padd(mod, fn, b, cur, ap, offs_off, ti64, ln, lc);
    PirInst lo = pir_make_inst(PIR_OP_LOAD, ti32, ln, lc);
    lo.align = 4u; lo.operand_count = 1u; lo.operands = pir_alloc_ops(mod, 1u);
    lo.operands[0] = pir_op_val(op, tptr);
    lo.results[0]  = pir_fn_new_val(mod, fn, "vao", ti32, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, cur, lo);

    PirValueId cur_offs = lo.results[0];
    if (align_gp16) {
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

    if (is_hfa) {
        /* Copy each HFA element from its 16-byte __vr_top slot into packed dst_buf. */
        PirType ety = hfa_info.regs[0];
        PirTypeLayout ely = pir_type_layout(mod, target, ety);
        for (uint32_t k = 0u; k < hfa_info.reg_count; ++k) {
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
            st.align = ely.align; st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
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
    uint32_t stk_al = is_hfa ? ly.align : nat_al;
    if (!indirect && stk_al > 8u) {
        uint64_t al = (stk_al > 16u) ? 16u : stk_al;
        stk_ptr = pir_emit_align_ptr(mod, fn, mem_blk, mb, stk_ptr, al, target, ln, lc);
    }

    PirInst ns = pir_make_inst(PIR_OP_PTR_ADD, ti64, ln, lc);
    ns.operand_count = 2u; ns.operands = pir_alloc_ops(mod, 2u);
    ns.operands[0] = pir_op_val(stk_ptr, tptr);
    ns.operands[1] = pir_op_imm_i(step, false, ti64);
    ns.results[0]  = pir_fn_new_val(mod, fn, "vanstk", tptr, mem_blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, mb, ns);

    PirInst ss = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
    ss.align = 8u; ss.operand_count = 2u; ss.operands = pir_alloc_ops(mod, 2u);
    ss.operands[0] = pir_op_val(ns.results[0], tptr);
    ss.operands[1] = ap;
    pir_fn_append_inst(fn, mb, ss);

    if (is_hfa) {
        PirInst mc = pir_make_inst(PIR_OP_MEMCPY, tvoid, ln, lc);
        mc.align = ly.align < 8u ? ly.align : 8u;
        mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
        mc.operands[0] = inst->operands[0];
        mc.operands[1] = pir_op_val(stk_ptr, tptr);
        mc.operands[2] = pir_op_imm_i(ly.size, false, ti64);
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
    if (is_buf) {
        PirValueId src_ptr = res_ptr;
        if (indirect) {
            PirInst lp = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
            lp.align = 8u; lp.operand_count = 1u; lp.operands = pir_alloc_ops(mod, 1u);
            lp.operands[0] = pir_op_val(res_ptr, tptr);
            lp.results[0]  = pir_fn_new_val(mod, fn, "vaindp", tptr, cont_blk, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, &prep, lp);
            src_ptr = lp.results[0];
        }
        PirInst mc = pir_make_inst(PIR_OP_MEMCPY, tvoid, ln, lc);
        mc.align = ly.align < 8u ? ly.align : 8u;
        mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
        mc.operands[0] = inst->operands[0];
        mc.operands[1] = pir_op_val(src_ptr, tptr);
        mc.operands[2] = pir_op_imm_i(ly.size, false, ti64);
        pir_fn_append_inst(fn, &prep, mc);
    } else if (promote_f32) {
        PirInst ld = pir_make_inst(PIR_OP_LOAD, load_ty, ln, lc);
        ld.align = 8u;
        ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
        ld.operands[0] = pir_op_val(res_ptr, tptr);
        ld.results[0]  = pir_fn_new_val(mod, fn, "vaf64", load_ty, cont_blk, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, &prep, ld);

        PirInst ft = pir_make_inst(PIR_OP_FPTRUNC, arg_ty, ln, lc);
        ft.operand_count = 1u; ft.operands = pir_alloc_ops(mod, 1u);
        ft.operands[0] = pir_op_val(ld.results[0], load_ty);
        ft.results[0]  = inst->results[0];
        fn->values.data[inst->results[0]].def_block = cont_blk;
        pir_fn_append_inst(fn, &prep, ft);
    } else {
        PirValueId src_ptr = res_ptr;
        uint32_t ld_align = ly.align < 8u ? ly.align : 8u;
        if (indirect_val) {
            PirInst lp = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
            lp.align = 8u; lp.operand_count = 1u; lp.operands = pir_alloc_ops(mod, 1u);
            lp.operands[0] = pir_op_val(res_ptr, tptr);
            lp.results[0]  = pir_fn_new_val(mod, fn, "vaindp", tptr, cont_blk, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, &prep, lp);
            src_ptr = lp.results[0];
            ld_align = ly.align > 0u ? ly.align : 1u;
        }
        PirInst ld = pir_make_inst(PIR_OP_LOAD, arg_ty, ln, lc);
        ld.align = ld_align;
        ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
        ld.operands[0] = pir_op_val(src_ptr, tptr);
        ld.results[0]  = inst->results[0];
        fn->values.data[inst->results[0]].def_block = cont_blk;
        pir_fn_append_inst(fn, &prep, ld);
    }
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
    PirType tvoid  = pir_type_scalar(PIR_SCALAR_VOID);
    uint32_t ptr_al = target->ptr_bytes > 0u ? (uint32_t)target->ptr_bytes : 8u;
    uint64_t slot_sz = (target->ptr_bytes == 4u) ? 4u : 8u;
    uint32_t ln = inst->src_line, lc = inst->loc_idx;

    bool is_buf = (inst->op == PIR_OP_VA_ARG_BUF);
    PirType arg_ty = is_buf ? inst->aux_type : inst->ty;
    bool promote_f32 = (!is_buf && arg_ty.kind == PIR_TYPE_SCALAR && arg_ty.u.scalar == PIR_SCALAR_F32);
    PirType load_ty = promote_f32 ? pir_type_scalar(PIR_SCALAR_F64) : arg_ty;
    PirOperand ap  = is_buf ? inst->operands[1] : inst->operands[0];
    PirTypeLayout ly = pir_type_layout(mod, target, load_ty);
    if (is_buf) {
        if (ly.size == 0u) return;
        PirParamDecl pd;
        memset(&pd, 0, sizeof(pd));
        pd.abi_kind = PIR_PARAM_BYVAL;
        pd.abi_type = arg_ty;
        pd.type     = tptr;
        PirAbiRegState st0 = { 0u, 0u };
        PirAbiArgInfo info;
        if (pir_abi_classify_param(mod, target, &pd, &st0, &info) &&
            info.kind == PIR_ABI_DIRECT && info.reg_count == 0u) {
            return;
        }
    }
    bool indirect_val = (!is_buf &&
                         ((target->arch == PIR_ARCH_X86_64 && target->os == PIR_OS_WINDOWS && ly.size > 8u) ||
                          (target->arch == PIR_ARCH_ARM64 && load_ty.kind == PIR_TYPE_VECTOR && ly.size > 16u)));
    bool indirect = (is_buf && va_agg_is_indirect(mod, target, arg_ty)) || indirect_val;
    uint64_t step = indirect ? slot_sz : va_align_up(ly.size > 0u ? ly.size : slot_sz, slot_sz);

    PirInst lcur = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
    lcur.align = ptr_al; lcur.operand_count = 1u; lcur.operands = pir_alloc_ops(mod, 1u);
    lcur.operands[0] = ap;
    lcur.results[0]  = pir_fn_new_val(mod, fn, "vacur", tptr, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, lcur);

    PirValueId cur_ptr = lcur.results[0];
    if (target->arch == PIR_ARCH_ARM64 && target->os == PIR_OS_MACOS &&
        !indirect && ly.align > 8u) {
        /* Darwin ARM64 aligns direct variadic stack arguments with align > 8 up to 16B. */
        uint64_t al = (ly.align > 16u) ? 16u : ly.align;
        cur_ptr = pir_emit_align_ptr(mod, fn, b, out, cur_ptr, al, target, ln, lc);
    }

    PirInst next = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
    next.operand_count = 2u; next.operands = pir_alloc_ops(mod, 2u);
    next.operands[0] = pir_op_val(cur_ptr, tptr);
    next.operands[1] = pir_op_imm_i(step, false, idx_ty);
    next.results[0]  = pir_fn_new_val(mod, fn, "vanxt", tptr, b, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, next);

    PirInst scur = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
    scur.align = ptr_al; scur.operand_count = 2u; scur.operands = pir_alloc_ops(mod, 2u);
    scur.operands[0] = pir_op_val(next.results[0], tptr);
    scur.operands[1] = ap;
    pir_fn_append_inst(fn, out, scur);

    if (is_buf) {
        PirValueId src_ptr = cur_ptr;
        if (indirect) {
            PirInst lp = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
            lp.align = ptr_al; lp.operand_count = 1u; lp.operands = pir_alloc_ops(mod, 1u);
            lp.operands[0] = pir_op_val(cur_ptr, tptr);
            lp.results[0]  = pir_fn_new_val(mod, fn, "vaindp", tptr, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, out, lp);
            src_ptr = lp.results[0];
        }
        PirInst mc = pir_make_inst(PIR_OP_MEMCPY, tvoid, ln, lc);
        mc.align = ly.align < ptr_al ? ly.align : ptr_al;
        mc.operand_count = 3u; mc.operands = pir_alloc_ops(mod, 3u);
        mc.operands[0] = inst->operands[0];
        mc.operands[1] = pir_op_val(src_ptr, tptr);
        mc.operands[2] = pir_op_imm_i(ly.size, false, idx_ty);
        pir_fn_append_inst(fn, out, mc);
    } else if (promote_f32) {
        PirInst ld = pir_make_inst(PIR_OP_LOAD, load_ty, ln, lc);
        ld.align = ptr_al < 8u ? ptr_al : 8u;
        ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
        ld.operands[0] = pir_op_val(cur_ptr, tptr);
        ld.results[0]  = pir_fn_new_val(mod, fn, "vaf64", load_ty, b, PIR_INVALID_ID, 0u);
        pir_fn_append_inst(fn, out, ld);

        PirInst ft = pir_make_inst(PIR_OP_FPTRUNC, arg_ty, ln, lc);
        ft.operand_count = 1u; ft.operands = pir_alloc_ops(mod, 1u);
        ft.operands[0] = pir_op_val(ld.results[0], load_ty);
        ft.results[0]  = inst->results[0];
        pir_fn_append_inst(fn, out, ft);
    } else {
        PirValueId src_ptr = cur_ptr;
        uint32_t ld_align = ly.align < ptr_al ? ly.align : ptr_al;
        if (indirect_val) {
            PirInst lp = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
            lp.align = ptr_al; lp.operand_count = 1u; lp.operands = pir_alloc_ops(mod, 1u);
            lp.operands[0] = pir_op_val(cur_ptr, tptr);
            lp.results[0]  = pir_fn_new_val(mod, fn, "vaindp", tptr, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, out, lp);
            src_ptr = lp.results[0];
            ld_align = ly.align > 0u ? ly.align : 1u;
        }
        PirInst ld = pir_make_inst(PIR_OP_LOAD, arg_ty, ln, lc);
        ld.align = ld_align;
        ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
        ld.operands[0] = pir_op_val(src_ptr, tptr);
        ld.results[0]  = inst->results[0];
        pir_fn_append_inst(fn, out, ld);
    }
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

bool pir_lower_varargs(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    (void)diag;
    bool is_sysv_x64     = (target->arch == PIR_ARCH_X86_64 && target->os != PIR_OS_WINDOWS);
    bool is_aapcs64_lin  = (target->arch == PIR_ARCH_ARM64  && target->os != PIR_OS_MACOS);
    bool is_ptr_cursor   = !is_sysv_x64 && !is_aapcs64_lin; /* Darwin ARM64, Win64, 32-bit */
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
                            ld.operands[0].call_abi = PIR_PARAM_NORMAL;
                            ld.operands[0].has_explicit_type = false;
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
        if (prologue.len > 0u) {
            for (uint32_t j = 0u; j < f->blocks.data[0].insts.len; ++j) {
                PIR_VEC_PUSH(&prologue, f->blocks.data[0].insts.data[j]);
            }
            PIR_VEC_FREE(&f->blocks.data[0].insts);
            f->blocks.data[0].insts = prologue;
        }
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
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            bool has_ov = false;
            for (uint32_t j = 0u; j < fn->blocks.data[b].insts.len; ++j) {
                const PirInst *ins = &fn->insts.data[fn->blocks.data[b].insts.data[j]];
                if (ins->op == PIR_OP_VA_ARG && ins->ty.kind == PIR_TYPE_VECTOR &&
                    pir_vec_bits(ins->ty.u.vec) > target->caps.max_simd_bits) {
                    uint16_t vbits = pir_vec_bits(ins->ty.u.vec);
                    if ((target->arch == PIR_ARCH_X86_64 && target->os == PIR_OS_WINDOWS && vbits > 64u) ||
                        (target->arch == PIR_ARCH_ARM64 && vbits > 128u) ||
                        (target->arch == PIR_ARCH_X86_64 && target->os != PIR_OS_WINDOWS && vbits > 128u)) {
                        continue;
                    }
                    has_ov = true;
                    break;
                }
            }
            if (!has_ov) continue;
            PirInstIdVec out = {0};
            for (uint32_t j = 0u; j < fn->blocks.data[b].insts.len; ++j) {
                PirInstId iid = fn->blocks.data[b].insts.data[j];
                PirInst copy  = fn->insts.data[iid];
                bool do_split = (copy.op == PIR_OP_VA_ARG && copy.ty.kind == PIR_TYPE_VECTOR &&
                                 pir_vec_bits(copy.ty.u.vec) > target->caps.max_simd_bits);
                if (do_split) {
                    uint16_t vbits = pir_vec_bits(copy.ty.u.vec);
                    if ((target->arch == PIR_ARCH_X86_64 && target->os == PIR_OS_WINDOWS && vbits > 64u) ||
                        (target->arch == PIR_ARCH_ARM64 && vbits > 128u) ||
                        (target->arch == PIR_ARCH_X86_64 && target->os != PIR_OS_WINDOWS && vbits > 128u)) {
                        do_split = false;
                    }
                }
                if (do_split) {
                    PirType pty;
                    uint32_t cnt = 0u;
                    va_split_vec_spec(copy.ty.u.vec, target->caps.max_simd_bits, &pty, &cnt);
                    PirTypeLayout vly = pir_type_layout(mod, target, copy.ty);
                    PirTypeLayout ply = pir_type_layout(mod, target, pty);
                    uint32_t ln = copy.src_line, lc = copy.loc_idx;

                    PirInst al = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, ln, lc);
                    al.imm_u64 = vly.size; al.align = vly.align;
                    al.results[0] = pir_fn_new_val(mod, fn, "vav", tptr, b, PIR_INVALID_ID, 0u);
                    pir_fn_append_inst(fn, &out, al);

                    for (uint32_t p = 0u; p < cnt; ++p) {
                        PirInst va = pir_make_inst(PIR_OP_VA_ARG, pty, ln, lc);
                        va.operand_count = 1u; va.operands = pir_alloc_ops(mod, 1u);
                        va.operands[0] = copy.operands[0];
                        va.results[0]  = pir_fn_new_val(mod, fn, "vav_p", pty, b, PIR_INVALID_ID, 0u);
                        pir_fn_append_inst(fn, &out, va);

                        PirValueId sp = va_padd(mod, fn, b, &out, pir_op_val(al.results[0], tptr),
                                                (uint64_t)p * ply.size, idx_ty, ln, lc);
                        PirInst st = pir_make_inst(PIR_OP_STORE, pty, ln, lc);
                        st.align = ply.align; st.operand_count = 2u; st.operands = pir_alloc_ops(mod, 2u);
                        st.operands[0] = pir_op_val(va.results[0], pty);
                        st.operands[1] = pir_op_val(sp, tptr);
                        pir_fn_append_inst(fn, &out, st);
                    }
                    PirInst ld = pir_make_inst(PIR_OP_LOAD, copy.ty, ln, lc);
                    ld.align = vly.align; ld.operand_count = 1u; ld.operands = pir_alloc_ops(mod, 1u);
                    ld.operands[0] = pir_op_val(al.results[0], tptr);
                    ld.results[0]  = copy.results[0];
                    pir_fn_append_inst(fn, &out, ld);
                } else {
                    PIR_VEC_PUSH(&out, iid);
                }
            }
            PIR_VEC_FREE(&fn->blocks.data[b].insts);
            fn->blocks.data[b].insts = out;
        }
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
