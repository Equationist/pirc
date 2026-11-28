/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * AVR 8-bit / 16-bit-pair (ATmega1280, avr-unknown-elf) Instruction Selection (LIR -> MIR).
 *
 * All LIR values after soft-float + i64 + i32 legalization are i8, i16, or ptr (16-bit).
 * Each GP vreg holds a 16-bit AVR register pair r(2p):r(2p+1).
 */

#include "analysis/cfg.h"
#include "backend/backend.h"
#include "lower/lower.h"
#include "opt/opt.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const PirModule     *mod;
    const PirFunction   *fn;
    const PirTargetSpec *target;
    MFunc               *mf;
    PirDiag             *diag;
    bool                 ok;

    MVReg    *val_vreg;
    uint32_t *alloca_slot;
    uint32_t *blk_map;
    uint32_t  cur_mblk;

    /* Hidden multi-reg return buffer vreg (for PIR_CABI_RET_MULTI_REG). */
    MVReg     multi_ret_vreg;
} AvrISelCtx;

static uint8_t avr_val_width(PirType ty) {
    if (ty.kind == PIR_TYPE_SCALAR && ty.u.scalar == PIR_SCALAR_I8) return 1u;
    return 2u;
}

static void emit(AvrISelCtx *c, MInst ins) {
    mblock_push(&c->mf->blocks.data[c->cur_mblk], ins);
}

static MVReg new_gp(AvrISelCtx *c) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_GP, 2u);
}

static uint16_t eval_imm_u16(const PirOperand *op) {
    uint64_t raw = op->u.imm_int.is_negative ? (UINT64_C(0) - op->u.imm_int.lo) : op->u.imm_int.lo;
    return (uint16_t)(raw & 0xFFFFu);
}

static void emit_li(AvrISelCtx *c, MVReg dst, uint16_t val, uint32_t loc) {
    MInst li = minst_make(MI_AVR_LI, 2u, loc);
    li.op_count = 2u;
    li.ops[0]   = mop_vreg_def(dst);
    li.ops[1]   = mop_imm((int64_t)val);
    emit(c, li);
}

static void emit_copy(AvrISelCtx *c, MVReg dst, MVReg src, uint32_t loc) {
    MInst cp = minst_make(MI_COPY, 2u, loc);
    cp.op_count = 2u;
    cp.ops[0]   = mop_vreg_def(dst);
    cp.ops[1]   = mop_vreg_use(src);
    emit(c, cp);
}

static void emit_zext8(AvrISelCtx *c, MVReg dst, MVReg src, uint32_t loc) {
    MInst z = minst_make(MI_AVR_ZEXT8, 2u, loc);
    z.op_count = 2u;
    z.ops[0]   = mop_vreg_def(dst);
    z.ops[1]   = mop_vreg_use(src);
    emit(c, z);
}

static void emit_sext8(AvrISelCtx *c, MVReg dst, MVReg src, uint32_t loc) {
    MInst s = minst_make(MI_AVR_SEXT8, 2u, loc);
    s.op_count = 2u;
    s.ops[0]   = mop_vreg_def(dst);
    s.ops[1]   = mop_vreg_use(src);
    emit(c, s);
}

static void lookup_symbol_meta(const PirModule *mod, const char *name,
                               bool *out_is_func, bool *out_is_tls) {
    *out_is_func = false;
    *out_is_tls  = false;
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        if (strcmp(mod->funcs.data[i].name, name) == 0) {
            *out_is_func = true;
            return;
        }
    }
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        if (strcmp(mod->globals.data[i].name, name) == 0) {
            *out_is_tls = (mod->globals.data[i].seg == PIR_GSEG_TLS);
            return;
        }
    }
}

static void emit_sym_addr(AvrISelCtx *c, MVReg dst, const char *name, int64_t offset, uint32_t loc) {
    bool is_func = false, is_tls = false;
    lookup_symbol_meta(c->mod, name, &is_func, &is_tls);
    MInst sa = minst_make(MI_AVR_SYM_ADDR, 2u, loc);
    sa.op_count = 2u;
    sa.ops[0]   = mop_vreg_def(dst);
    sa.ops[1]   = mop_sym(name, is_func ? 0 : offset, is_func, is_tls, false);
    emit(c, sa);
    if (is_func && offset != 0) {
        MVReg t = new_gp(c);
        emit_copy(c, t, dst, loc);
        MInst ad = minst_make(MI_AVR_ADDI, 2u, loc);
        ad.op_count = 3u;
        ad.ops[0] = mop_vreg_def(dst);
        ad.ops[1] = mop_vreg_use(t);
        ad.ops[2] = mop_imm((int64_t)(int16_t)offset);
        emit(c, ad);
    }
}

static MVReg mat_op(AvrISelCtx *c, const PirOperand *op, uint32_t loc) {
    switch (op->kind) {
        case PIR_OPER_VAL:
            return c->val_vreg[op->u.val];
        case PIR_OPER_IMM_INT: {
            MVReg r = new_gp(c);
            emit_li(c, r, eval_imm_u16(op), loc);
            return r;
        }
        case PIR_OPER_IMM_FLOAT: {
            MVReg r = new_gp(c);
            emit_li(c, r, (uint16_t)(op->u.imm_float.bits & 0xFFFFu), loc);
            return r;
        }
        case PIR_OPER_SYM: {
            MVReg r = new_gp(c);
            emit_sym_addr(c, r, op->u.sym.name, op->u.sym.offset, loc);
            return r;
        }
        case PIR_OPER_INVOKE_RET:
        case PIR_OPER_INVOKE_EXN:
            break;
    }
    return new_gp(c);
}

/* Emit trap if cond_reg != 0 (width 1) by branching around a synthetic trap block. */
static void emit_trap_if_nz(AvrISelCtx *c, MVReg cond_i8, uint32_t loc) {
    uint32_t trap_mb = mfunc_new_block(c->mf, NULL);
    uint32_t cont_mb = mfunc_new_block(c->mf, NULL);

    MInst br = minst_make(MI_AVR_BR_NZ, 1u, loc);
    br.op_count = 2u; br.ops[0] = mop_vreg_use(cond_i8); br.ops[1] = mop_block(trap_mb);
    emit(c, br);

    MInst jc = minst_make(MI_AVR_JMP, 2u, loc);
    jc.op_count = 1u; jc.ops[0] = mop_block(cont_mb);
    emit(c, jc);

    mfunc_add_edge(c->mf, c->cur_mblk, trap_mb);
    mfunc_add_edge(c->mf, c->cur_mblk, cont_mb);

    c->cur_mblk = trap_mb;
    emit(c, minst_make(MI_AVR_TRAP, 2u, loc));

    c->cur_mblk = cont_mb;
}

/* Emit trap if (lhs <cond> rhs) is true. */
static void emit_trap_if_cmp(AvrISelCtx *c, uint8_t w, MCond cc, MVReg lhs, MVReg rhs, uint32_t loc) {
    uint32_t trap_mb = mfunc_new_block(c->mf, NULL);
    uint32_t cont_mb = mfunc_new_block(c->mf, NULL);

    MInst br = minst_make(MI_AVR_BR_CMP, w, loc);
    br.op_count = 4u;
    br.ops[0] = mop_vreg_use(lhs);
    br.ops[1] = mop_vreg_use(rhs);
    br.ops[2] = mop_cond(cc);
    br.ops[3] = mop_block(trap_mb);
    emit(c, br);

    MInst jc = minst_make(MI_AVR_JMP, 2u, loc);
    jc.op_count = 1u; jc.ops[0] = mop_block(cont_mb);
    emit(c, jc);

    mfunc_add_edge(c->mf, c->cur_mblk, trap_mb);
    mfunc_add_edge(c->mf, c->cur_mblk, cont_mb);

    c->cur_mblk = trap_mb;
    emit(c, minst_make(MI_AVR_TRAP, 2u, loc));

    c->cur_mblk = cont_mb;
}

/* =========================================================================
 * AVR Argument Placement Planner (Shared by Entry Params & Call Sites)
 * ========================================================================= */

typedef struct {
    MPReg   preg;   /* AVR_PAIR(4..12) if in register, else MREG_NONE */
    int64_t soff;   /* Non-negative byte offset on stack if on stack, else -1 */
} AvrArgLoc;

static uint64_t avr_plan_args(uint32_t n, bool is_variadic,
                              const uint8_t *arg_w,
                              const uint8_t *group_words, const bool *force_stack,
                              AvrArgLoc *locs) {
    uint64_t stk_off = 0u;
    if (is_variadic) {
        /* AVR C ABI: ALL arguments (fixed + variadic) pass on stack (1-byte stack align). */
        for (uint32_t i = 0u; i < n; ++i) {
            locs[i].preg = MREG_NONE;
            locs[i].soff = (int64_t)stk_off;
            stk_off += (arg_w && arg_w[i] > 0u) ? (uint64_t)arg_w[i] : 2u;
        }
        return stk_off;
    }

    uint32_t next_reg = 26u; /* Pairs allocated top-down from r24:r25 down to r8:r9 */
    for (uint32_t i = 0u; i < n; ) {
        uint32_t gw = (group_words && group_words[i] >= 2u && i + group_words[i] <= n)
                      ? (uint32_t)group_words[i] : 1u;
        bool fstk = (force_stack && force_stack[i]);
        if (!fstk && next_reg >= 8u + 2u * gw) {
            uint32_t base_reg = next_reg - 2u * gw;
            next_reg = base_reg;
            for (uint32_t k = 0u; k < gw; ++k) {
                locs[i + k].preg = AVR_PAIR((base_reg / 2u) + k);
                locs[i + k].soff = -1;
            }
        } else {
            next_reg = 8u; /* Once spilled to stack, all remaining args pass on stack */
            for (uint32_t k = 0u; k < gw; ++k) {
                locs[i + k].preg = MREG_NONE;
                locs[i + k].soff = (int64_t)stk_off;
                stk_off += (arg_w && arg_w[i + k] > 0u) ? (uint64_t)arg_w[i + k] : 2u;
            }
        }
        i += gw;
    }
    return stk_off;
}

/* =========================================================================
 * Block-Parameter Parallel Copies & Edge Splitting
 * ========================================================================= */

static void emit_branch_pcopy(AvrISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
    if (bt->arg_count == 0u) return;
    const PirBlock *dst_blk = &c->fn->blocks.data[bt->target];
    MPCopyEntry *ents = (MPCopyEntry *)calloc(bt->arg_count, sizeof(MPCopyEntry));
    if (!ents) abort();
    uint32_t cnt = 0u;

    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        PirValueId dv = dst_blk->params.data[k];
        const PirOperand *aop = &bt->args[k];
        ents[cnt].dst       = c->val_vreg[dv];
        ents[cnt].rclass    = MREG_CLASS_GP;
        ents[cnt].byte_size = 2u;
        if (aop->kind == PIR_OPER_IMM_INT) {
            ents[cnt].src_is_imm = true;
            ents[cnt].imm_bits   = (uint64_t)eval_imm_u16(aop);
        } else {
            ents[cnt].src_is_imm = false;
            ents[cnt].src        = mat_op(c, aop, loc);
        }
        cnt++;
    }
    if (cnt > 0u) {
        uint32_t pid = mfunc_add_pcopy(c->mf, ents, cnt);
        MInst pc = minst_make(MI_PCOPY, 2u, loc);
        pc.aux_id = pid;
        emit(c, pc);
    }
    free(ents);
}

static uint32_t resolve_edge_target(AvrISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
    uint32_t dst_mb = c->blk_map[bt->target];
    if (bt->arg_count == 0u) return dst_mb;
    uint32_t tramp = mfunc_new_block(c->mf, NULL);
    uint32_t saved = c->cur_mblk;
    c->cur_mblk = tramp;
    emit_branch_pcopy(c, bt, loc);
    MInst j = minst_make(MI_AVR_JMP, 2u, loc);
    j.op_count = 1u; j.ops[0] = mop_block(dst_mb);
    emit(c, j);
    mfunc_add_edge(c->mf, tramp, dst_mb);
    c->cur_mblk = saved;
    return tramp;
}

/* =========================================================================
 * Calls (Direct, Indirect, Multi-Reg Returns, Variadic Stack Args)
 * ========================================================================= */

static void isel_call(AvrISelCtx *c, const PirInst *inst, uint32_t loc) {
    c->mf->has_calls = true;
    bool is_ind = (inst->op == PIR_OP_CALL_INDIRECT);

    PirCabiRetInfo cabi_ret;
    memset(&cabi_ret, 0, sizeof(cabi_ret));
    bool is_variadic = false;
    const PirParamDecl *callee_params = NULL;
    uint32_t callee_param_count = 0u;

    if (!is_ind && inst->operands[0].u.sym.is_func &&
        inst->operands[0].u.sym.id < c->mod->funcs.len) {
        const PirFunction *cal = &c->mod->funcs.data[inst->operands[0].u.sym.id];
        cabi_ret           = cal->cabi_ret;
        is_variadic        = cal->is_variadic;
        callee_params      = cal->params;
        callee_param_count = cal->param_count;
    } else if (is_ind && inst->sig_id < c->mod->sigs.len) {
        const PirSigDef *sig = &c->mod->sigs.data[inst->sig_id];
        cabi_ret           = sig->cabi_ret;
        is_variadic        = sig->is_variadic;
        callee_params      = sig->params;
        callee_param_count = sig->param_count;
    }

    uint32_t first_arg = 1u;
    MVReg multi_ret_buf = MVREG_NONE;
    if (cabi_ret.kind == PIR_CABI_RET_MULTI_REG && inst->operand_count >= 2u) {
        multi_ret_buf = mat_op(c, &inst->operands[1], loc);
        first_arg = 2u;
    }

    uint32_t nargs = (inst->operand_count > first_arg) ? (inst->operand_count - first_arg) : 0u;
    MVReg     *arg_v  = nargs ? (MVReg *)calloc(nargs, sizeof(MVReg)) : NULL;
    uint8_t   *arg_w  = nargs ? (uint8_t *)calloc(nargs, sizeof(uint8_t)) : NULL;
    uint8_t   *arg_gw = nargs ? (uint8_t *)calloc(nargs, sizeof(uint8_t)) : NULL;
    bool      *arg_fs = nargs ? (bool *)calloc(nargs, sizeof(bool)) : NULL;
    AvrArgLoc *locs   = nargs ? (AvrArgLoc *)calloc(nargs, sizeof(AvrArgLoc)) : NULL;
    if (nargs && (!arg_v || !arg_w || !arg_gw || !arg_fs || !locs)) abort();

    for (uint32_t i = 0u; i < nargs; ++i) {
        const PirOperand *aop = &inst->operands[first_arg + i];
        arg_v[i]  = mat_op(c, aop, loc);
        arg_w[i]  = avr_val_width(aop->type);
        uint32_t pi = (first_arg - 1u) + i;
        uint8_t gw = aop->call_group_words;
        if (gw == 0u && callee_params && pi < callee_param_count) {
            gw = callee_params[pi].abi_group_words;
        }
        arg_gw[i] = gw;
        arg_fs[i] = aop->call_force_stack ||
                    (callee_params && pi < callee_param_count && callee_params[pi].abi_force_stack);

        /* On AVR variadic stack, widen ONLY variadic (non-fixed) i8 args to 16-bit
         * (C default int promotion); fixed i8 params occupy 1 byte on the stack. */
        if (is_variadic && pi >= callee_param_count && arg_w[i] == 1u) {
            MVReg wv = new_gp(c);
            emit_zext8(c, wv, arg_v[i], loc);
            arg_v[i] = wv;
            arg_w[i] = 2u;
        }
    }

    uint64_t out_bytes = avr_plan_args(nargs, is_variadic, arg_w, arg_gw, arg_fs, locs);
    if (out_bytes > c->mf->max_out_args_bytes) c->mf->max_out_args_bytes = out_bytes;

    if (out_bytes > 0u) {
        MInst adj = minst_make(MI_CALL_ADJ_DOWN, 2u, loc);
        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)out_bytes);
        emit(c, adj);
    }

    /* Emit outgoing stack arg stores first. */
    for (uint32_t i = 0u; i < nargs; ++i) {
        if (locs[i].soff < 0) continue;
        MInst st = minst_make(MI_STORE_OUTARG, arg_w[i], loc);
        st.op_count = 2u;
        st.ops[0]   = mop_vreg_use(arg_v[i]);
        st.ops[1]   = mop_imm(locs[i].soff);
        emit(c, st);
    }

    MVReg ind_v = MVREG_NONE;
    if (is_ind) {
        ind_v = mat_op(c, &inst->operands[0], loc);
    }

    /* Emit parallel copy for all register arguments (handles cycles and r8..r17 cleanly). */
    uint32_t nreg_args = 0u;
    for (uint32_t i = 0u; i < nargs; ++i) {
        if (locs[i].preg != MREG_NONE) nreg_args++;
    }
    if (nreg_args > 0u) {
        MPCopyEntry *ents = (MPCopyEntry *)calloc(nreg_args, sizeof(MPCopyEntry));
        if (!ents) abort();
        uint32_t k = 0u;
        for (uint32_t i = 0u; i < nargs; ++i) {
            if (locs[i].preg == MREG_NONE) continue;
            MVReg fv = new_gp(c);
            c->mf->vregs.data[fv].fixed_preg = locs[i].preg;
            ents[k].dst        = fv;
            ents[k].src_is_imm = false;
            ents[k].src        = arg_v[i];
            ents[k].rclass     = MREG_CLASS_GP;
            ents[k].byte_size  = 2u;
            k++;
        }
        uint32_t pid = mfunc_add_pcopy(c->mf, ents, nreg_args);
        MInst pc = minst_make(MI_PCOPY, 2u, loc);
        pc.aux_id = pid;
        emit(c, pc);
        free(ents);
    }

    if (is_ind) {
        MInst ci = minst_make(MI_AVR_ICALL, 2u, loc);
        ci.is_call  = true;
        ci.op_count = 1u;
        ci.ops[0]   = mop_vreg_use(ind_v);
        emit(c, ci);
    } else {
        MInst cl = minst_make(MI_AVR_CALL, 2u, loc);
        cl.is_call  = true;
        cl.op_count = 1u;
        cl.ops[0]   = mop_sym(inst->operands[0].u.sym.name, 0, true, false, false);
        emit(c, cl);
    }

    if (out_bytes > 0u) {
        MInst adj = minst_make(MI_CALL_ADJ_UP, 2u, loc);
        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)out_bytes);
        emit(c, adj);
    }

    free(arg_v);
    free(arg_w);
    free(arg_gw);
    free(arg_fs);
    free(locs);

    /* Capture return value(s). */
    if (cabi_ret.kind == PIR_CABI_RET_MULTI_REG) {
        uint32_t nregs = cabi_ret.reg_count;
        if (nregs > 4u) nregs = 4u;
        /* AVR-GCC / Clang rounds multi-reg return sizes up to powers of 2:
         * 1..2B -> r24.. (pair 12), 3..4B -> r22.. (pair 11), 5..8B -> r18.. (pair 9). */
        uint32_t base_pair = (nregs >= 3u) ? 9u : (13u - nregs);
        MVReg saved[4] = { MVREG_NONE, MVREG_NONE, MVREG_NONE, MVREG_NONE };
        for (uint32_t k = 0u; k < nregs; ++k) {
            saved[k] = new_gp(c);
            MInst cp = minst_make(MI_COPY, 2u, loc);
            cp.op_count = 2u;
            cp.ops[0] = mop_vreg_def(saved[k]);
            cp.ops[1] = mop_preg_use(AVR_PAIR(base_pair + k));
            emit(c, cp);
        }
        for (uint32_t k = 0u; k < nregs; ++k) {
            uint64_t byte_off = (uint64_t)k * 2u;
            uint8_t sw = (cabi_ret.agg_size > 0u && byte_off + 1u == cabi_ret.agg_size) ? 1u : 2u;
            MInst st = minst_make(MI_AVR_ST, sw, loc);
            st.op_count = 3u;
            st.ops[0] = mop_vreg_use(saved[k]);
            st.ops[1] = mop_vreg_use(multi_ret_buf);
            st.ops[2] = mop_imm((int64_t)byte_off);
            emit(c, st);
        }
    } else if (inst->results[0] != PIR_INVALID_ID) {
        MVReg dst = c->val_vreg[inst->results[0]];
        MInst cp = minst_make(MI_COPY, 2u, loc);
        cp.op_count = 2u;
        cp.ops[0] = mop_vreg_def(dst);
        cp.ops[1] = mop_preg_use(AVR_R24R25);
        emit(c, cp);
    }
}

/* =========================================================================
 * 16-bit & 8-bit Software Division / Remainder via __*hi3 Helpers
 * ========================================================================= */

static void isel_div_rem(AvrISelCtx *c, const PirInst *inst, uint32_t loc) {
    c->mf->has_calls = true;
    uint8_t w = avr_val_width(inst->ty);
    bool is_s   = (inst->op == PIR_OP_DIV_S || inst->op == PIR_OP_REM_S);
    bool is_rem = (inst->op == PIR_OP_REM_S || inst->op == PIR_OP_REM_U);
    MVReg dst = c->val_vreg[inst->results[0]];
    MVReg a   = mat_op(c, &inst->operands[0], loc);
    MVReg b   = mat_op(c, &inst->operands[1], loc);

    if (w == 1u) {
        MVReg ae = new_gp(c), be = new_gp(c);
        if (is_s) { emit_sext8(c, ae, a, loc); emit_sext8(c, be, b, loc); }
        else      { emit_zext8(c, ae, a, loc); emit_zext8(c, be, b, loc); }
        a = ae; b = be;
    }

    if (!(inst->flags & PIR_FLAG_UNSAFE)) {
        MVReg z = new_gp(c);
        emit_li(c, z, 0u, loc);
        emit_trap_if_cmp(c, 2u, MCOND_EQ, b, z, loc);
    }

    MVReg is_min_m1 = MVREG_NONE;
    MVReg call_b = b;
    if (is_s && !(inst->flags & PIR_FLAG_UNSAFE)) {
        uint16_t min_u = (w == 1u) ? (uint16_t)0xFF80u : (uint16_t)0x8000u;
        MVReg min_r = new_gp(c), m1_r = new_gp(c);
        emit_li(c, min_r, min_u, loc);
        emit_li(c, m1_r, 0xFFFFu, loc);
        MVReg eq_min = new_gp(c), eq_m1 = new_gp(c);
        MInst c1 = minst_make(MI_AVR_CMP_SET, 2u, loc);
        c1.op_count = 4u; c1.ops[0] = mop_vreg_def(eq_min); c1.ops[1] = mop_vreg_use(a);
        c1.ops[2] = mop_vreg_use(min_r); c1.ops[3] = mop_cond(MCOND_EQ);
        emit(c, c1);
        MInst c2 = minst_make(MI_AVR_CMP_SET, 2u, loc);
        c2.op_count = 4u; c2.ops[0] = mop_vreg_def(eq_m1); c2.ops[1] = mop_vreg_use(b);
        c2.ops[2] = mop_vreg_use(m1_r); c2.ops[3] = mop_cond(MCOND_EQ);
        emit(c, c2);
        is_min_m1 = new_gp(c);
        MInst an = minst_make(MI_AVR_AND, 1u, loc);
        an.op_count = 3u; an.ops[0] = mop_vreg_def(is_min_m1);
        an.ops[1] = mop_vreg_use(eq_min); an.ops[2] = mop_vreg_use(eq_m1);
        emit(c, an);
        if (inst->flags & PIR_FLAG_WRAP) {
            MVReg one_r = new_gp(c);
            emit_li(c, one_r, 1u, loc);
            call_b = new_gp(c);
            MInst sl = minst_make(MI_AVR_SELECT, 2u, loc);
            sl.op_count = 4u; sl.ops[0] = mop_vreg_def(call_b);
            sl.ops[1] = mop_vreg_use(is_min_m1); sl.ops[2] = mop_vreg_use(one_r); sl.ops[3] = mop_vreg_use(b);
            emit(c, sl);
        } else {
            emit_trap_if_nz(c, is_min_m1, loc);
        }
    }

    const char *hname = (inst->op == PIR_OP_DIV_S) ? "__divhi3"
                      : (inst->op == PIR_OP_DIV_U) ? "__udivhi3"
                      : (inst->op == PIR_OP_REM_S) ? "__modhi3" : "__umodhi3";

    MVReg fa = new_gp(c); c->mf->vregs.data[fa].fixed_preg = AVR_R24R25;
    MVReg fb = new_gp(c); c->mf->vregs.data[fb].fixed_preg = AVR_R22R23;
    MPCopyEntry ents[2];
    ents[0].dst = fa; ents[0].src_is_imm = false; ents[0].src = a;      ents[0].rclass = MREG_CLASS_GP; ents[0].byte_size = 2u;
    ents[1].dst = fb; ents[1].src_is_imm = false; ents[1].src = call_b; ents[1].rclass = MREG_CLASS_GP; ents[1].byte_size = 2u;
    uint32_t pid = mfunc_add_pcopy(c->mf, ents, 2u);
    MInst pc = minst_make(MI_PCOPY, 2u, loc);
    pc.aux_id = pid;
    emit(c, pc);

    MInst cl = minst_make(MI_AVR_CALL, 2u, loc);
    cl.is_call = true; cl.op_count = 1u;
    cl.ops[0]  = mop_sym(hname, 0, true, false, false);
    emit(c, cl);

    if (is_s && (inst->flags & PIR_FLAG_WRAP) && is_min_m1 != MVREG_NONE) {
        MVReg raw_res = new_gp(c);
        MInst cp = minst_make(MI_COPY, 2u, loc);
        cp.op_count = 2u; cp.ops[0] = mop_vreg_def(raw_res); cp.ops[1] = mop_preg_use(AVR_R24R25);
        emit(c, cp);
        MVReg wv = new_gp(c);
        uint16_t wrap_val = is_rem ? 0u : ((w == 1u) ? (uint16_t)0x0080u : (uint16_t)0x8000u);
        emit_li(c, wv, wrap_val, loc);
        MInst sl = minst_make(MI_AVR_SELECT, 2u, loc);
        sl.op_count = 4u; sl.ops[0] = mop_vreg_def(dst);
        sl.ops[1] = mop_vreg_use(is_min_m1); sl.ops[2] = mop_vreg_use(wv); sl.ops[3] = mop_vreg_use(raw_res);
        emit(c, sl);
    } else {
        MInst cp = minst_make(MI_COPY, 2u, loc);
        cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(AVR_R24R25);
        emit(c, cp);
    }
}

/* =========================================================================
 * Instruction Selection Main Dispatcher
 * ========================================================================= */

static MCond pir_cmp_to_mcond(PirOpcode op) {
    if (op == PIR_OP_CMP_EQ)   return MCOND_EQ;
    if (op == PIR_OP_CMP_NE)   return MCOND_NE;
    if (op == PIR_OP_CMP_LT_S) return MCOND_LT;
    if (op == PIR_OP_CMP_LE_S) return MCOND_LE;
    if (op == PIR_OP_CMP_GT_S) return MCOND_GT;
    if (op == PIR_OP_CMP_GE_S) return MCOND_GE;
    if (op == PIR_OP_CMP_LT_U) return MCOND_CC;
    if (op == PIR_OP_CMP_LE_U) return MCOND_LS;
    if (op == PIR_OP_CMP_GT_U) return MCOND_HI;
    if (op == PIR_OP_CMP_GE_U) return MCOND_CS;
    return MCOND_EQ;
}

static void emit_mul_wide_pinned(AvrISelCtx *c, MVReg rlo, MVReg rhi,
                                 MVReg lhs, MVReg rhs, bool is_signed, uint32_t loc) {
    /* Pin all 4 operands to distinct fixed GP pairs so regalloc never needs 4 GP scratches
     * when both early-clobber outputs and both inputs are spilled simultaneously. */
    MVReg p_lhs = new_gp(c); c->mf->vregs.data[p_lhs].fixed_preg = AVR_R24R25;
    MVReg p_rhs = new_gp(c); c->mf->vregs.data[p_rhs].fixed_preg = AVR_R22R23;
    MVReg p_rlo = new_gp(c); c->mf->vregs.data[p_rlo].fixed_preg = AVR_R20R21;
    MVReg p_rhi = new_gp(c); c->mf->vregs.data[p_rhi].fixed_preg = AVR_R18R19;
    MPCopyEntry ents[2];
    ents[0].dst = p_lhs; ents[0].src_is_imm = false; ents[0].src = lhs; ents[0].rclass = MREG_CLASS_GP; ents[0].byte_size = 2u;
    ents[1].dst = p_rhs; ents[1].src_is_imm = false; ents[1].src = rhs; ents[1].rclass = MREG_CLASS_GP; ents[1].byte_size = 2u;
    uint32_t pid = mfunc_add_pcopy(c->mf, ents, 2u);
    MInst pc = minst_make(MI_PCOPY, 2u, loc);
    pc.aux_id = pid;
    emit(c, pc);

    MInst mw = minst_make(MI_AVR_MUL_WIDE, 2u, loc);
    mw.is_signed = is_signed;
    mw.op_count  = 4u;
    mw.ops[0]    = mop_vreg_def_ec(p_rlo);
    mw.ops[1]    = mop_vreg_def_ec(p_rhi);
    mw.ops[2]    = mop_vreg_use(p_lhs);
    mw.ops[3]    = mop_vreg_use(p_rhs);
    emit(c, mw);

    emit_copy(c, rlo, p_rlo, loc);
    emit_copy(c, rhi, p_rhi, loc);
}

static void isel_inst(AvrISelCtx *c, PirInstId iid, const PirInst *inst) {
    uint32_t loc = inst->loc_idx;

    switch (inst->op) {
        case PIR_OP_CONST: {
            MVReg dst = c->val_vreg[inst->results[0]];
            if (inst->operands[0].kind == PIR_OPER_SYM) {
                emit_sym_addr(c, dst, inst->operands[0].u.sym.name,
                              inst->operands[0].u.sym.offset, loc);
            } else {
                emit_li(c, dst, eval_imm_u16(&inst->operands[0]), loc);
            }
            return;
        }

        case PIR_OP_ADDR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            emit_sym_addr(c, dst, inst->operands[0].u.sym.name, inst->operands[0].u.sym.offset, loc);
            return;
        }

        case PIR_OP_ALLOCA_PTR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MInst fa = minst_make(MI_FRAME_ADDR, 2u, loc);
            fa.op_count = 3u;
            fa.ops[0] = mop_vreg_def(dst);
            fa.ops[1] = mop_slot(c->alloca_slot[iid]);
            fa.ops[2] = mop_imm(0);
            emit(c, fa);
            return;
        }

        case PIR_OP_ALLOCA_DYN: {
            c->mf->has_alloca_dyn = true;
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg sz  = mat_op(c, &inst->operands[0], loc);
            MVReg sp  = new_gp(c);
            MInst isp = minst_make(MI_AVR_IN_SP, 2u, loc);
            isp.op_count = 1u; isp.ops[0] = mop_vreg_def(sp);
            emit(c, isp);
            MVReg nsp = new_gp(c);
            MInst sb = minst_make(MI_AVR_SUB, 2u, loc);
            sb.op_count = 3u; sb.ops[0] = mop_vreg_def(nsp); sb.ops[1] = mop_vreg_use(sp); sb.ops[2] = mop_vreg_use(sz);
            emit(c, sb);
            MInst osp = minst_make(MI_AVR_OUT_SP, 2u, loc);
            osp.op_count = 1u; osp.ops[0] = mop_vreg_use(nsp);
            emit(c, osp);
            /* AVR SP points 1 byte below the top of stack -> buffer starts at SP + 1. */
            MInst ad = minst_make(MI_AVR_ADDI, 2u, loc);
            ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst); ad.ops[1] = mop_vreg_use(nsp); ad.ops[2] = mop_imm(1);
            emit(c, ad);
            return;
        }

        case PIR_OP_STACKSAVE: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MInst isp = minst_make(MI_AVR_IN_SP, 2u, loc);
            isp.op_count = 1u; isp.ops[0] = mop_vreg_def(dst);
            emit(c, isp);
            return;
        }

        case PIR_OP_STACKRESTORE: {
            MVReg sp = mat_op(c, &inst->operands[0], loc);
            MInst osp = minst_make(MI_AVR_OUT_SP, 2u, loc);
            osp.op_count = 1u; osp.ops[0] = mop_vreg_use(sp);
            emit(c, osp);
            return;
        }

        case PIR_OP_PTR_ADD: {
            MVReg dst  = c->val_vreg[inst->results[0]];
            MVReg base = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint16_t off = eval_imm_u16(&inst->operands[1]);
                if (off == 0u) {
                    emit_copy(c, dst, base, loc);
                } else {
                    MInst ad = minst_make(MI_AVR_ADDI, 2u, loc);
                    ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst);
                    ad.ops[1] = mop_vreg_use(base); ad.ops[2] = mop_imm((int64_t)(int16_t)off);
                    emit(c, ad);
                }
            } else {
                MVReg off = mat_op(c, &inst->operands[1], loc);
                if (inst->ty.kind == PIR_TYPE_SCALAR && inst->ty.u.scalar == PIR_SCALAR_I8) {
                    MVReg se = new_gp(c);
                    emit_sext8(c, se, off, loc);
                    off = se;
                }
                MInst ad = minst_make(MI_AVR_ADD, 2u, loc);
                ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst);
                ad.ops[1] = mop_vreg_use(base); ad.ops[2] = mop_vreg_use(off);
                emit(c, ad);
            }
            return;
        }

        case PIR_OP_LOAD: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            uint8_t w = avr_val_width(inst->ty);
            MInst ld = minst_make(MI_AVR_LD, w, loc);
            ld.op_count = 3u;
            ld.ops[0] = mop_vreg_def(dst);
            ld.ops[1] = mop_vreg_use(ptr);
            ld.ops[2] = mop_imm(0);
            emit(c, ld);
            return;
        }

        case PIR_OP_STORE: {
            MVReg val = mat_op(c, &inst->operands[0], loc);
            MVReg ptr = mat_op(c, &inst->operands[1], loc);
            uint8_t w = avr_val_width(inst->ty);
            MInst st = minst_make(MI_AVR_ST, w, loc);
            st.op_count = 3u;
            st.ops[0] = mop_vreg_use(val);
            st.ops[1] = mop_vreg_use(ptr);
            st.ops[2] = mop_imm(0);
            emit(c, st);
            return;
        }

        case PIR_OP_ADD:
        case PIR_OP_SUB: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = avr_val_width(inst->ty);
            bool is_add = (inst->op == PIR_OP_ADD);
            bool has_trap = (inst->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u;
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            if (!has_trap && inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint16_t imm = eval_imm_u16(&inst->operands[1]);
                int16_t simm = is_add ? (int16_t)imm : (int16_t)(uint16_t)(0u - imm);
                MInst ai = minst_make(MI_AVR_ADDI, w, loc);
                ai.op_count = 3u; ai.ops[0] = mop_vreg_def(dst);
                ai.ops[1] = mop_vreg_use(lhs); ai.ops[2] = mop_imm((int64_t)simm);
                emit(c, ai);
                return;
            }
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            MInst op = minst_make(is_add ? MI_AVR_ADD : MI_AVR_SUB, w, loc);
            op.op_count = 3u; op.ops[0] = mop_vreg_def(dst);
            op.ops[1] = mop_vreg_use(lhs); op.ops[2] = mop_vreg_use(rhs);
            emit(c, op);
            if (inst->flags & PIR_FLAG_TRAP_U) {
                if (is_add) emit_trap_if_cmp(c, w, MCOND_CC, dst, lhs, loc);
                else        emit_trap_if_cmp(c, w, MCOND_CC, lhs, rhs, loc);
            } else if (inst->flags & PIR_FLAG_TRAP_S) {
                MVReg x1 = new_gp(c), x2 = new_gp(c), x3 = new_gp(c), z = new_gp(c);
                MInst m1 = minst_make(MI_AVR_XOR, w, loc);
                m1.op_count = 3u; m1.ops[0] = mop_vreg_def(x1);
                m1.ops[1] = mop_vreg_use(is_add ? dst : lhs);
                m1.ops[2] = mop_vreg_use(is_add ? lhs : rhs);
                emit(c, m1);
                MInst m2 = minst_make(MI_AVR_XOR, w, loc);
                m2.op_count = 3u; m2.ops[0] = mop_vreg_def(x2);
                m2.ops[1] = mop_vreg_use(is_add ? dst : lhs);
                m2.ops[2] = mop_vreg_use(is_add ? rhs : dst);
                emit(c, m2);
                MInst m3 = minst_make(MI_AVR_AND, w, loc);
                m3.op_count = 3u; m3.ops[0] = mop_vreg_def(x3);
                m3.ops[1] = mop_vreg_use(x1); m3.ops[2] = mop_vreg_use(x2);
                emit(c, m3);
                emit_li(c, z, 0u, loc);
                emit_trap_if_cmp(c, w, MCOND_LT, x3, z, loc);
            }
            return;
        }

        case PIR_OP_MUL: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = avr_val_width(inst->ty);
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            if ((inst->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u) {
                bool is_s = (inst->flags & PIR_FLAG_TRAP_S) != 0u;
                if (w == 2u) {
                    MVReg hi = new_gp(c);
                    emit_mul_wide_pinned(c, dst, hi, lhs, rhs, is_s, loc);
                    if (is_s) {
                        MVReg sgn = new_gp(c);
                        MInst sr = minst_make(MI_AVR_SARI, 2u, loc);
                        sr.op_count = 3u; sr.ops[0] = mop_vreg_def(sgn);
                        sr.ops[1] = mop_vreg_use(dst); sr.ops[2] = mop_imm(15);
                        emit(c, sr);
                        emit_trap_if_cmp(c, 2u, MCOND_NE, hi, sgn, loc);
                    } else {
                        MVReg z = new_gp(c);
                        emit_li(c, z, 0u, loc);
                        emit_trap_if_cmp(c, 2u, MCOND_NE, hi, z, loc);
                    }
                } else {
                    MVReg a16 = new_gp(c), b16 = new_gp(c), prod = new_gp(c);
                    if (is_s) { emit_sext8(c, a16, lhs, loc); emit_sext8(c, b16, rhs, loc); }
                    else      { emit_zext8(c, a16, lhs, loc); emit_zext8(c, b16, rhs, loc); }
                    MInst m = minst_make(MI_AVR_MUL, 2u, loc);
                    m.op_count = 3u; m.ops[0] = mop_vreg_def_ec(prod);
                    m.ops[1] = mop_vreg_use(a16); m.ops[2] = mop_vreg_use(b16);
                    emit(c, m);
                    emit_copy(c, dst, prod, loc);
                    MVReg chk = new_gp(c);
                    if (is_s) emit_sext8(c, chk, prod, loc);
                    else      emit_zext8(c, chk, prod, loc);
                    emit_trap_if_cmp(c, 2u, MCOND_NE, prod, chk, loc);
                }
                return;
            }
            MInst m = minst_make(MI_AVR_MUL, w, loc);
            m.op_count = 3u; m.ops[0] = mop_vreg_def_ec(dst);
            m.ops[1] = mop_vreg_use(lhs); m.ops[2] = mop_vreg_use(rhs);
            emit(c, m);
            return;
        }

        case PIR_OP_MUL_WIDE_U:
        case PIR_OP_MUL_WIDE_S: {
            MVReg rlo = c->val_vreg[inst->results[0]];
            MVReg rhi = c->val_vreg[inst->results[1]];
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            emit_mul_wide_pinned(c, rlo, rhi, lhs, rhs, (inst->op == PIR_OP_MUL_WIDE_S), loc);
            return;
        }

        case PIR_OP_DIV_S:
        case PIR_OP_DIV_U:
        case PIR_OP_REM_S:
        case PIR_OP_REM_U:
            isel_div_rem(c, inst, loc);
            return;

        case PIR_OP_AND:
        case PIR_OP_OR:
        case PIR_OP_XOR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = avr_val_width(inst->ty);
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                MOpcode op = (inst->op == PIR_OP_AND) ? MI_AVR_ANDI
                           : (inst->op == PIR_OP_OR)  ? MI_AVR_ORI : MI_AVR_XORI;
                MInst ins = minst_make(op, w, loc);
                ins.op_count = 3u; ins.ops[0] = mop_vreg_def(dst);
                ins.ops[1] = mop_vreg_use(lhs); ins.ops[2] = mop_imm((int64_t)eval_imm_u16(&inst->operands[1]));
                emit(c, ins);
            } else {
                MVReg rhs = mat_op(c, &inst->operands[1], loc);
                MOpcode op = (inst->op == PIR_OP_AND) ? MI_AVR_AND
                           : (inst->op == PIR_OP_OR)  ? MI_AVR_OR : MI_AVR_XOR;
                MInst ins = minst_make(op, w, loc);
                ins.op_count = 3u; ins.ops[0] = mop_vreg_def(dst);
                ins.ops[1] = mop_vreg_use(lhs); ins.ops[2] = mop_vreg_use(rhs);
                emit(c, ins);
            }
            return;
        }

        case PIR_OP_SHL:
        case PIR_OP_SHR:
        case PIR_OP_SAR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = avr_val_width(inst->ty);
            uint16_t bits = (w == 1u) ? 8u : 16u;
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint16_t amt = eval_imm_u16(&inst->operands[1]);
                if (w == 1u) amt &= 0xFFu;
                if ((inst->flags & PIR_FLAG_TRAP) && amt >= bits) {
                    emit(c, minst_make(MI_AVR_TRAP, 2u, loc));
                    return;
                }
                amt &= (uint16_t)(bits - 1u);
                if (amt == 0u) {
                    emit_copy(c, dst, lhs, loc);
                    return;
                }
                MOpcode op = (inst->op == PIR_OP_SHL) ? MI_AVR_SHLI
                           : (inst->op == PIR_OP_SHR) ? MI_AVR_SHRI : MI_AVR_SARI;
                MInst sh = minst_make(op, w, loc);
                sh.op_count = 3u; sh.ops[0] = mop_vreg_def(dst);
                sh.ops[1] = mop_vreg_use(lhs); sh.ops[2] = mop_imm((int64_t)amt);
                emit(c, sh);
            } else {
                MVReg rhs = mat_op(c, &inst->operands[1], loc);
                if (inst->flags & PIR_FLAG_TRAP) {
                    MVReg lim = new_gp(c);
                    emit_li(c, lim, bits, loc);
                    emit_trap_if_cmp(c, w, MCOND_CS, rhs, lim, loc);
                }
                MVReg masked = new_gp(c);
                MInst an = minst_make(MI_AVR_ANDI, 1u, loc);
                an.op_count = 3u; an.ops[0] = mop_vreg_def(masked);
                an.ops[1] = mop_vreg_use(rhs); an.ops[2] = mop_imm((int64_t)(bits - 1u));
                emit(c, an);
                MOpcode op = (inst->op == PIR_OP_SHL) ? MI_AVR_SHL
                           : (inst->op == PIR_OP_SHR) ? MI_AVR_SHR : MI_AVR_SAR;
                MInst sh = minst_make(op, w, loc);
                sh.op_count = 3u; sh.ops[0] = mop_vreg_def_ec(dst);
                sh.ops[1] = mop_vreg_use(lhs); sh.ops[2] = mop_vreg_use(masked);
                emit(c, sh);
            }
            return;
        }

        case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LE_S: case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GE_S:
        case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_U: case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_U: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = avr_val_width(inst->ty);
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            MInst cm = minst_make(MI_AVR_CMP_SET, w, loc);
            cm.op_count = 4u;
            cm.ops[0] = mop_vreg_def(dst);
            cm.ops[1] = mop_vreg_use(lhs);
            cm.ops[2] = mop_vreg_use(rhs);
            cm.ops[3] = mop_cond(pir_cmp_to_mcond(inst->op));
            emit(c, cm);
            return;
        }

        case PIR_OP_SELECT: {
            MVReg dst  = c->val_vreg[inst->results[0]];
            MVReg cond = mat_op(c, &inst->operands[0], loc);
            MVReg tv   = mat_op(c, &inst->operands[1], loc);
            MVReg fv   = mat_op(c, &inst->operands[2], loc);
            MInst sl = minst_make(MI_AVR_SELECT, 2u, loc);
            sl.op_count = 4u;
            sl.ops[0] = mop_vreg_def(dst);
            sl.ops[1] = mop_vreg_use(cond);
            sl.ops[2] = mop_vreg_use(tv);
            sl.ops[3] = mop_vreg_use(fv);
            emit(c, sl);
            return;
        }

        case PIR_OP_ZEXT: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            emit_zext8(c, dst, src, loc);
            return;
        }

        case PIR_OP_SEXT: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            emit_sext8(c, dst, src, loc);
            return;
        }

        case PIR_OP_TRUNC:
        case PIR_OP_PTRTOINT:
        case PIR_OP_INTTOPTR:
        case PIR_OP_BITCAST: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            emit_copy(c, dst, src, loc);
            return;
        }

        case PIR_OP_ATOMIC_LOAD: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            uint8_t w = avr_val_width(inst->ty);
            MInst al = minst_make(MI_AVR_ATOMIC_LD, w, loc);
            al.op_count = 2u; al.ops[0] = mop_vreg_def_ec(dst); al.ops[1] = mop_vreg_use(ptr);
            emit(c, al);
            return;
        }

        case PIR_OP_ATOMIC_STORE: {
            MVReg val = mat_op(c, &inst->operands[0], loc);
            MVReg ptr = mat_op(c, &inst->operands[1], loc);
            uint8_t w = avr_val_width(inst->ty);
            MInst as = minst_make(MI_AVR_ATOMIC_ST, w, loc);
            as.op_count = 2u; as.ops[0] = mop_vreg_use(val); as.ops[1] = mop_vreg_use(ptr);
            emit(c, as);
            return;
        }

        case PIR_OP_ATOMIC_RMW: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MVReg val = mat_op(c, &inst->operands[1], loc);
            uint8_t w = avr_val_width(inst->ty);
            /* Pin to r24:r25, r22:r23, r20:r21 so X/Z scratch never collide. */
            MVReg p_old = new_gp(c); c->mf->vregs.data[p_old].fixed_preg = AVR_R24R25;
            MVReg p_ptr = new_gp(c); c->mf->vregs.data[p_ptr].fixed_preg = AVR_R22R23;
            MVReg p_val = new_gp(c); c->mf->vregs.data[p_val].fixed_preg = AVR_R20R21;
            MPCopyEntry ents[2];
            ents[0].dst = p_ptr; ents[0].src_is_imm = false; ents[0].src = ptr; ents[0].rclass = MREG_CLASS_GP; ents[0].byte_size = 2u;
            ents[1].dst = p_val; ents[1].src_is_imm = false; ents[1].src = val; ents[1].rclass = MREG_CLASS_GP; ents[1].byte_size = 2u;
            uint32_t pid = mfunc_add_pcopy(c->mf, ents, 2u);
            MInst pc = minst_make(MI_PCOPY, 2u, loc); pc.aux_id = pid; emit(c, pc);

            MInst rmw = minst_make(MI_AVR_ATOMIC_RMW, w, loc);
            rmw.aux_id   = (uint32_t)inst->rmw_op;
            rmw.op_count = 3u;
            rmw.ops[0] = mop_vreg_def_ec(p_old);
            rmw.ops[1] = mop_vreg_use(p_ptr);
            rmw.ops[2] = mop_vreg_use(p_val);
            emit(c, rmw);
            emit_copy(c, dst, p_old, loc);
            return;
        }

        case PIR_OP_ATOMIC_CMPXCHG: {
            MVReg old_v = c->val_vreg[inst->results[0]];
            MVReg ok_v  = c->val_vreg[inst->results[1]];
            MVReg ptr   = mat_op(c, &inst->operands[0], loc);
            MVReg exp_v = mat_op(c, &inst->operands[1], loc);
            MVReg des_v = mat_op(c, &inst->operands[2], loc);
            uint8_t w   = avr_val_width(inst->ty);

            MVReg p_old = new_gp(c); c->mf->vregs.data[p_old].fixed_preg = AVR_R24R25;
            MVReg p_ok  = new_gp(c); c->mf->vregs.data[p_ok].fixed_preg  = AVR_R22R23;
            MVReg p_ptr = new_gp(c); c->mf->vregs.data[p_ptr].fixed_preg = AVR_R20R21;
            MVReg p_exp = new_gp(c); c->mf->vregs.data[p_exp].fixed_preg = AVR_R18R19;
            MVReg p_des = new_gp(c); c->mf->vregs.data[p_des].fixed_preg = AVR_R14R15;
            MPCopyEntry ents[3];
            ents[0].dst = p_ptr; ents[0].src_is_imm = false; ents[0].src = ptr;   ents[0].rclass = MREG_CLASS_GP; ents[0].byte_size = 2u;
            ents[1].dst = p_exp; ents[1].src_is_imm = false; ents[1].src = exp_v; ents[1].rclass = MREG_CLASS_GP; ents[1].byte_size = 2u;
            ents[2].dst = p_des; ents[2].src_is_imm = false; ents[2].src = des_v; ents[2].rclass = MREG_CLASS_GP; ents[2].byte_size = 2u;
            uint32_t pid = mfunc_add_pcopy(c->mf, ents, 3u);
            MInst pc = minst_make(MI_PCOPY, 2u, loc); pc.aux_id = pid; emit(c, pc);

            MInst cas = minst_make(MI_AVR_ATOMIC_CAS, w, loc);
            cas.op_count = 5u;
            cas.ops[0] = mop_vreg_def_ec(p_old);
            cas.ops[1] = mop_vreg_def_ec(p_ok);
            cas.ops[2] = mop_vreg_use(p_ptr);
            cas.ops[3] = mop_vreg_use(p_exp);
            cas.ops[4] = mop_vreg_use(p_des);
            emit(c, cas);
            emit_copy(c, old_v, p_old, loc);
            emit_copy(c, ok_v, p_ok, loc);
            return;
        }

        case PIR_OP_FENCE:
            return; /* Single-core AVR is strictly ordered */

        case PIR_OP_VA_START: {
            MVReg ap = mat_op(c, &inst->operands[0], loc);
            MInst vs = minst_make(MI_VA_START, 2u, loc);
            vs.op_count = 1u; vs.ops[0] = mop_vreg_use(ap);
            emit(c, vs);
            return;
        }

        case PIR_OP_CALL:
        case PIR_OP_CALL_INDIRECT:
            isel_call(c, inst, loc);
            return;

        case PIR_OP_JMP: {
            emit_branch_pcopy(c, &inst->targets[0], loc);
            uint32_t dst_mb = c->blk_map[inst->targets[0].target];
            MInst j = minst_make(MI_AVR_JMP, 2u, loc);
            j.op_count = 1u; j.ops[0] = mop_block(dst_mb);
            emit(c, j);
            mfunc_add_edge(c->mf, c->cur_mblk, dst_mb);
            return;
        }

        case PIR_OP_BR: {
            MVReg cond = mat_op(c, &inst->operands[0], loc);
            uint32_t t_mb = resolve_edge_target(c, &inst->targets[0], loc);
            uint32_t f_mb = resolve_edge_target(c, &inst->targets[1], loc);
            MInst br = minst_make(MI_AVR_BR_NZ, 1u, loc);
            br.op_count = 2u; br.ops[0] = mop_vreg_use(cond); br.ops[1] = mop_block(t_mb);
            emit(c, br);
            MInst jf = minst_make(MI_AVR_JMP, 2u, loc);
            jf.op_count = 1u; jf.ops[0] = mop_block(f_mb);
            emit(c, jf);
            mfunc_add_edge(c->mf, c->cur_mblk, t_mb);
            mfunc_add_edge(c->mf, c->cur_mblk, f_mb);
            return;
        }

        case PIR_OP_SWITCH: {
            MVReg val = mat_op(c, &inst->operands[0], loc);
            uint8_t w = avr_val_width(inst->ty);
            uint32_t nc = inst->switch_case_count;
            uint32_t def_mb = resolve_edge_target(c, &inst->targets[0], loc);

            if (nc == 0u) {
                MInst j = minst_make(MI_AVR_JMP, 2u, loc);
                j.op_count = 1u; j.ops[0] = mop_block(def_mb);
                emit(c, j);
                mfunc_add_edge(c->mf, c->cur_mblk, def_mb);
                return;
            }

            /* Sign-extend/zero-extend scrutinee to 16-bit for dense jump table check. */
            int64_t min_v = INT64_MAX, max_v = INT64_MIN;
            for (uint32_t k = 0u; k < nc; ++k) {
                int64_t kv = inst->switch_cases[k].is_negative
                             ? -(int64_t)inst->switch_cases[k].case_lo
                             : (int64_t)inst->switch_cases[k].case_lo;
                if (w == 1u) kv = (int64_t)(int8_t)(uint8_t)kv;
                else         kv = (int64_t)(int16_t)(uint16_t)kv;
                if (kv < min_v) min_v = kv;
                if (kv > max_v) max_v = kv;
            }
            uint64_t span = (uint64_t)(max_v - min_v) + 1u;
            if (nc >= 4u && span <= 128u && span <= (uint64_t)nc * 3u) {
                MVReg v16 = val;
                if (w == 1u) {
                    v16 = new_gp(c);
                    emit_sext8(c, v16, val, loc);
                }
                uint32_t *tbl = (uint32_t *)malloc((size_t)span * sizeof(uint32_t));
                if (!tbl) abort();
                for (uint64_t i = 0u; i < span; ++i) tbl[i] = def_mb;
                for (uint32_t k = 0u; k < nc; ++k) {
                    int64_t kv = inst->switch_cases[k].is_negative
                                 ? -(int64_t)inst->switch_cases[k].case_lo
                                 : (int64_t)inst->switch_cases[k].case_lo;
                    if (w == 1u) kv = (int64_t)(int8_t)(uint8_t)kv;
                    else         kv = (int64_t)(int16_t)(uint16_t)kv;
                    uint32_t t_mb = resolve_edge_target(c, &inst->switch_cases[k].target, loc);
                    tbl[(uint64_t)(kv - min_v)] = t_mb;
                    mfunc_add_edge(c->mf, c->cur_mblk, t_mb);
                }
                mfunc_add_edge(c->mf, c->cur_mblk, def_mb);
                uint32_t jtid = mfunc_add_jtable(c->mf, min_v, (uint32_t)span, tbl, def_mb);
                free(tbl);

                MVReg idx = new_gp(c);
                MInst sub = minst_make(MI_AVR_ADDI, 2u, loc);
                sub.op_count = 3u; sub.ops[0] = mop_vreg_def(idx);
                sub.ops[1] = mop_vreg_use(v16); sub.ops[2] = mop_imm(-(int64_t)(int16_t)min_v);
                emit(c, sub);

                MInst sw = minst_make(MI_AVR_SWITCH_TBL, 2u, loc);
                sw.op_count = 3u;
                sw.ops[0] = mop_vreg_use(idx);
                sw.ops[1] = mop_jtable(jtid);
                sw.ops[2] = mop_block(def_mb);
                emit(c, sw);
                return;
            }

            /* Sparse switch: compare-and-branch chain. */
            for (uint32_t k = 0u; k < nc; ++k) {
                uint16_t kv = (uint16_t)(inst->switch_cases[k].is_negative
                                         ? (UINT64_C(0) - inst->switch_cases[k].case_lo)
                                         : inst->switch_cases[k].case_lo);
                uint32_t t_mb = resolve_edge_target(c, &inst->switch_cases[k].target, loc);
                MVReg kr = new_gp(c);
                emit_li(c, kr, kv, loc);
                MInst br = minst_make(MI_AVR_BR_CMP, w, loc);
                br.op_count = 4u;
                br.ops[0] = mop_vreg_use(val);
                br.ops[1] = mop_vreg_use(kr);
                br.ops[2] = mop_cond(MCOND_EQ);
                br.ops[3] = mop_block(t_mb);
                emit(c, br);
                mfunc_add_edge(c->mf, c->cur_mblk, t_mb);
            }
            MInst jd = minst_make(MI_AVR_JMP, 2u, loc);
            jd.op_count = 1u; jd.ops[0] = mop_block(def_mb);
            emit(c, jd);
            mfunc_add_edge(c->mf, c->cur_mblk, def_mb);
            return;
        }

        case PIR_OP_RET: {
            if (c->fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG) {
                uint32_t nregs = c->fn->cabi_ret.reg_count;
                if (nregs > 4u) nregs = 4u;
                uint32_t base_pair = (nregs >= 3u) ? 9u : (13u - nregs);
                MVReg loaded[4] = { MVREG_NONE, MVREG_NONE, MVREG_NONE, MVREG_NONE };
                for (uint32_t k = 0u; k < nregs; ++k) {
                    uint64_t byte_off = (uint64_t)k * 2u;
                    uint8_t lw = (c->fn->cabi_ret.agg_size > 0u && byte_off + 1u == c->fn->cabi_ret.agg_size) ? 1u : 2u;
                    loaded[k] = new_gp(c);
                    MInst ld = minst_make(MI_AVR_LD, lw, loc);
                    ld.op_count = 3u;
                    ld.ops[0] = mop_vreg_def(loaded[k]);
                    ld.ops[1] = mop_vreg_use(c->multi_ret_vreg);
                    ld.ops[2] = mop_imm((int64_t)byte_off);
                    emit(c, ld);
                }
                for (uint32_t k = 0u; k < nregs; ++k) {
                    MInst cp = minst_make(MI_COPY, 2u, loc);
                    cp.op_count = 2u;
                    cp.ops[0] = mop_preg_def(AVR_PAIR(base_pair + k));
                    cp.ops[1] = mop_vreg_use(loaded[k]);
                    emit(c, cp);
                }
            } else if (inst->operand_count == 1u) {
                MVReg rv = mat_op(c, &inst->operands[0], loc);
                MInst cp = minst_make(MI_COPY, 2u, loc);
                cp.op_count = 2u;
                cp.ops[0] = mop_preg_def(AVR_R24R25);
                cp.ops[1] = mop_vreg_use(rv);
                emit(c, cp);
            }
            emit(c, minst_make(MI_AVR_RET, 2u, loc));
            return;
        }

        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE:
            emit(c, minst_make(MI_AVR_TRAP, 2u, loc));
            return;

        /* Opcodes eliminated before AVR ISel (HIR, soft-float, i64, i32, SIMD, optional ops) */
        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
        case PIR_OP_CLZ: case PIR_OP_CTZ: case PIR_OP_POPCNT: case PIR_OP_BSWAP:
        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV: case PIR_OP_FNEG:
        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT: case PIR_OP_FCMP_OLE:
        case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE: case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE:
        case PIR_OP_FCMP_ULT: case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE:
        case PIR_OP_FPEXT: case PIR_OP_FPTRUNC:
        case PIR_OP_SITOFP: case PIR_OP_UITOFP: case PIR_OP_FPTOSI: case PIR_OP_FPTOUI:
        case PIR_OP_SPLAT: case PIR_OP_EXTRACT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
        case PIR_OP_ALLOCA: case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR:
        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
        case PIR_OP_VA_ARG: case PIR_OP_VA_ARG_BUF: case PIR_OP_VA_COPY: case PIR_OP_VA_END:
        case PIR_OP_INVOKE: case PIR_OP_INVOKE_INDIRECT: case PIR_OP_RESUME:
            pir_diag_error(c->diag, inst->src_line, 0u,
                           "AVR backend: unsupported opcode %u reached ISel", (unsigned)inst->op);
            c->ok = false;
            return;
    }
}

/* =========================================================================
 * Public Entry Point
 * ========================================================================= */

bool pir_avr_isel_func(const PirModule *mod, const PirFunction *fn, uint32_t fn_index,
                       const PirTargetSpec *target, MFunc *out_mf, PirDiag *diag) {
    mfunc_init(out_mf, fn, fn_index, target);
    if (!fn->is_defined || fn->blocks.len == 0u) return true;

    AvrISelCtx c;
    memset(&c, 0, sizeof(c));
    c.mod            = mod;
    c.fn             = fn;
    c.target         = target;
    c.mf             = out_mf;
    c.diag           = diag;
    c.ok             = true;
    c.multi_ret_vreg = MVREG_NONE;

    uint32_t nv = fn->values.len;
    uint32_t ni = fn->insts.len;
    uint32_t nb = fn->blocks.len;

    c.val_vreg    = (MVReg *)malloc(((size_t)nv + 1u) * sizeof(MVReg));
    c.alloca_slot = (uint32_t *)calloc((size_t)ni + 1u, sizeof(uint32_t));
    c.blk_map     = (uint32_t *)malloc(((size_t)nb + 1u) * sizeof(uint32_t));
    if (!c.val_vreg || !c.alloca_slot || !c.blk_map) abort();

    for (uint32_t v = 0u; v < nv; ++v) {
        c.val_vreg[v] = new_gp(&c);
    }
    for (uint32_t b = 0u; b < nb; ++b) {
        c.blk_map[b] = mfunc_new_block(out_mf, fn->blocks.data[b].label);
    }
    for (uint32_t i = 0u; i < ni; ++i) {
        if (fn->insts.data[i].op == PIR_OP_ALLOCA_PTR) {
            uint64_t sz = fn->insts.data[i].imm_u64 > 0u ? fn->insts.data[i].imm_u64 : 1u;
            c.alloca_slot[i] = mfunc_add_slot(out_mf, sz, 1u, false);
        }
    }

    /* Emit entry parameter marshaling in block 0. */
    c.cur_mblk = c.blk_map[0];
    uint32_t start_p = 0u;
    if (fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG && fn->param_count >= 1u) {
        uint64_t rsz = fn->cabi_ret.agg_size > 0u ? fn->cabi_ret.agg_size : (uint64_t)fn->cabi_ret.reg_count * 2u;
        uint32_t rslot = mfunc_add_slot(out_mf, rsz, 1u, false);
        c.multi_ret_vreg = c.val_vreg[fn->params[0].val_id];
        MInst fa = minst_make(MI_FRAME_ADDR, 2u, 0u);
        fa.op_count = 3u;
        fa.ops[0] = mop_vreg_def(c.multi_ret_vreg);
        fa.ops[1] = mop_slot(rslot);
        fa.ops[2] = mop_imm(0);
        emit(&c, fa);
        start_p = 1u;
    }

    uint32_t np = (fn->param_count > start_p) ? (fn->param_count - start_p) : 0u;
    if (np > 0u) {
        uint8_t   *pw   = (uint8_t *)calloc(np, sizeof(uint8_t));
        uint8_t   *pgw  = (uint8_t *)calloc(np, sizeof(uint8_t));
        bool      *pfs  = (bool *)calloc(np, sizeof(bool));
        AvrArgLoc *locs = (AvrArgLoc *)calloc(np, sizeof(AvrArgLoc));
        if (!pw || !pgw || !pfs || !locs) abort();
        for (uint32_t i = 0u; i < np; ++i) {
            pw[i]  = avr_val_width(fn->params[start_p + i].type);
            pgw[i] = fn->params[start_p + i].abi_group_words;
            pfs[i] = fn->params[start_p + i].abi_force_stack;
        }
        uint64_t fixed_stk = avr_plan_args(np, fn->is_variadic, pw, pgw, pfs, locs);
        out_mf->va_fixed_stack_bytes = fixed_stk;

        /* Copy register arguments first while incoming ABI regs are untouched. */
        for (uint32_t i = 0u; i < np; ++i) {
            if (locs[i].preg == MREG_NONE) continue;
            MVReg dst = c.val_vreg[fn->params[start_p + i].val_id];
            out_mf->vregs.data[dst].hint_preg = locs[i].preg;
            MInst cp = minst_make(MI_COPY, 2u, 0u);
            cp.op_count = 2u;
            cp.ops[0] = mop_vreg_def(dst);
            cp.ops[1] = mop_preg_use(locs[i].preg);
            emit(&c, cp);
        }
        for (uint32_t i = 0u; i < np; ++i) {
            if (locs[i].soff < 0) continue;
            MVReg dst = c.val_vreg[fn->params[start_p + i].val_id];
            MInst ld = minst_make(MI_LOAD_INARG, pw[i], 0u);
            ld.op_count = 2u;
            ld.ops[0] = mop_vreg_def(dst);
            ld.ops[1] = mop_imm(locs[i].soff);
            emit(&c, ld);
        }
        free(pw);
        free(pgw);
        free(pfs);
        free(locs);
    }

    /* Select instructions for every PIR block. */
    for (uint32_t b = 0u; b < nb && c.ok; ++b) {
        c.cur_mblk = c.blk_map[b];
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len && c.ok; ++j) {
            PirInstId iid = blk->insts.data[j];
            isel_inst(&c, iid, &fn->insts.data[iid]);
        }
    }

    free(c.val_vreg);
    free(c.alloca_slot);
    free(c.blk_map);
    return c.ok;
}
