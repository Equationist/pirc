/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lower/lower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef bool (*PirExceptionLowerer)(PirModule *mod, const PirTargetSpec *target, PirDiag *diag);

static const char *EXC_HEAD_NAME = "__pir_exc_head";
static const char *SETJMP_NAME   = "__pir_setjmp";
static const char *RESUME_NAME   = "__pir_unwind_resume";

static void ensure_exc_head_global(PirModule *mod) {
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        if (strcmp(mod->globals.data[i].name, EXC_HEAD_NAME) == 0) return;
    }
    PirGlobal g;
    memset(&g, 0, sizeof(g));
    g.name        = pir_arena_strdup(&mod->arena, EXC_HEAD_NAME);
    g.linkage     = PIR_LINK_WEAK;
    g.seg         = PIR_GSEG_TLS;
    g.align       = 8u;
    g.has_type    = true;
    g.type        = pir_type_scalar(PIR_SCALAR_PTR);
    g.item_count  = 1u;
    g.items       = PIR_ARENA_NEW_ARRAY(&mod->arena, PirGlobalItem, 1u);
    g.items[0].kind   = PIR_GITEM_INT;
    g.items[0].int_lo = 0u;
    PIR_VEC_PUSH(&mod->globals, g);
}

static PirBlockId add_block(PirModule *mod, PirFunction *fn, const char *pfx, uint32_t line) {
    char buf[48];
    PirBlockId bid = fn->blocks.len;
    snprintf(buf, sizeof(buf), "%s_%u", pfx, bid);
    PirBlock blk;
    memset(&blk, 0, sizeof(blk));
    blk.label    = pir_arena_strdup(&mod->arena, buf);
    blk.src_line = line;
    PIR_VEC_PUSH(&fn->blocks, blk);
    return bid;
}

static bool target_uses_kind(const PirBranchTarget *bt, PirOperandKind kind) {
    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        if (bt->args[k].kind == kind) return true;
    }
    return false;
}

static PirBranchTarget clone_target_replace(PirModule *mod, const PirBranchTarget *bt,
                                            PirOperandKind kind, PirValueId repl_val,
                                            PirType repl_ty) {
    PirBranchTarget out = *bt;
    if (bt->arg_count > 0u) {
        out.args = pir_alloc_ops(mod, bt->arg_count);
        for (uint32_t k = 0u; k < bt->arg_count; ++k) {
            if (bt->args[k].kind == kind) {
                out.args[k] = pir_op_val(repl_val, repl_ty);
            } else {
                out.args[k] = bt->args[k];
            }
        }
    }
    return out;
}

static PirOperand make_global_sym(const char *name) {
    PirOperand o;
    memset(&o, 0, sizeof(o));
    o.kind       = PIR_OPER_SYM;
    o.type       = pir_type_scalar(PIR_SCALAR_PTR);
    o.u.sym.name = name;
    o.u.sym.id   = PIR_INVALID_ID;
    return o;
}

static bool lower_exceptions_shadow_stack(PirModule *mod, const PirTargetSpec *target,
                                          PirDiag *diag) {
    (void)diag;
    PirType tptr   = pir_type_scalar(PIR_SCALAR_PTR);
    PirType ti32   = pir_type_scalar(PIR_SCALAR_I32);
    PirType ti8    = pir_type_scalar(PIR_SCALAR_I8);
    PirType tvoid  = pir_type_scalar(PIR_SCALAR_VOID);
    PirType idx_ty = pir_type_scalar(pir_ptr_int_scalar(target->ptr_bytes));
    uint32_t pb    = target->ptr_bytes;

    /* Scan whether any throwing invoke or resume exists. */
    bool any_throwing_invoke = false;
    bool any_resume          = false;
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        const PirFunction *fn = &mod->funcs.data[fi];
        if (!fn->is_defined) continue;
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            const PirBlock *blk = &fn->blocks.data[b];
            for (uint32_t j = 0u; j < blk->insts.len; ++j) {
                const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
                if (ins->op == PIR_OP_INVOKE) {
                    const PirFunction *cal = &mod->funcs.data[ins->operands[0].u.sym.id];
                    if (!(cal->attrs & PIR_FATTR_NOUNWIND)) any_throwing_invoke = true;
                } else if (ins->op == PIR_OP_INVOKE_INDIRECT) {
                    const PirSigDef *sig = &mod->sigs.data[ins->sig_id];
                    if (!(sig->attrs & PIR_FATTR_NOUNWIND)) any_throwing_invoke = true;
                } else if (ins->op == PIR_OP_RESUME) {
                    any_resume = true;
                }
            }
        }
    }

    PirFuncId setjmp_id = PIR_INVALID_ID;
    PirFuncId resume_id = PIR_INVALID_ID;
    if (any_throwing_invoke) {
        ensure_exc_head_global(mod);
        PirType sj_p[1] = { tptr };
        setjmp_id = pir_ensure_extern_func(mod, SETJMP_NAME, sj_p, 1u, ti32);
    }
    if (any_resume) {
        PirType rs_p[1] = { tptr };
        resume_id = pir_ensure_extern_func(mod, RESUME_NAME, rs_p, 1u, tvoid);
        mod->funcs.data[resume_id].attrs = PIR_FATTR_NORETURN;
    }

    uint32_t orig_funcs = mod->funcs.len;
    for (uint32_t fi = 0u; fi < orig_funcs; ++fi) {
        PirFunction *fn = &mod->funcs.data[fi];
        if (!fn->is_defined) continue;

        /* Check if this function needs a shadow-stack frame in entry block. */
        bool fn_needs_frame = false;
        for (uint32_t b = 0u; b < fn->blocks.len && !fn_needs_frame; ++b) {
            const PirBlock *blk = &fn->blocks.data[b];
            if (blk->insts.len == 0u) continue;
            const PirInst *term = &fn->insts.data[blk->insts.data[blk->insts.len - 1u]];
            if (term->op == PIR_OP_INVOKE &&
                !(mod->funcs.data[term->operands[0].u.sym.id].attrs & PIR_FATTR_NOUNWIND)) {
                fn_needs_frame = true;
            } else if (term->op == PIR_OP_INVOKE_INDIRECT &&
                       !(mod->sigs.data[term->sig_id].attrs & PIR_FATTR_NOUNWIND)) {
                fn_needs_frame = true;
            }
        }

        PirValueId exc_frame = PIR_INVALID_ID;
        if (fn_needs_frame) {
            /* Prepend one frame slot { prev: ptr, exn: ptr, jmp_buf } to blocks[0]. */
            uint64_t jb_sz = (target->arch == PIR_ARCH_AVR) ? 32u : 384u;
            PirInst al = pir_make_inst(PIR_OP_ALLOCA_PTR, tptr, fn->src_line, 0u);
            al.imm_u64    = 2u * pb + jb_sz;
            al.align      = (target->arch == PIR_ARCH_AVR) ? 1u : 16u;
            al.results[0] = pir_fn_new_val(mod, fn, "exc_fr", tptr, 0u, PIR_INVALID_ID, 0u);
            PirInstId aid = pir_fn_append_inst(fn, NULL, al);
            exc_frame     = al.results[0];

            PirInstIdVec new_entry = {0};
            PIR_VEC_PUSH(&new_entry, aid);
            for (uint32_t j = 0u; j < fn->blocks.data[0].insts.len; ++j) {
                PIR_VEC_PUSH(&new_entry, fn->blocks.data[0].insts.data[j]);
            }
            PIR_VEC_FREE(&fn->blocks.data[0].insts);
            fn->blocks.data[0].insts = new_entry;
        }

        uint32_t orig_blocks = fn->blocks.len;
        for (uint32_t b = 0u; b < orig_blocks; ++b) {
            if (fn->blocks.data[b].insts.len == 0u) continue;
            uint32_t last_idx = fn->blocks.data[b].insts.len - 1u;
            PirInstId term_id = fn->blocks.data[b].insts.data[last_idx];
            PirInst copy      = fn->insts.data[term_id];
            uint32_t ln = copy.src_line, lc = copy.loc_idx;

            if (copy.op == PIR_OP_RESUME) {
                fn->blocks.data[b].insts.len = last_idx;
                PirInst call = pir_make_inst(PIR_OP_CALL, tvoid, ln, lc);
                call.operand_count = 2u;
                call.operands      = pir_alloc_ops(mod, 2u);
                call.operands[0]   = pir_op_sym_func(mod->funcs.data[resume_id].name, resume_id);
                call.operands[1]   = copy.operands[0];
                pir_fn_append_inst(fn, &fn->blocks.data[b].insts, call);
                PirInst unr = pir_make_inst(PIR_OP_UNREACHABLE, tvoid, ln, lc);
                pir_fn_append_inst(fn, &fn->blocks.data[b].insts, unr);
                continue;
            }

            if (copy.op != PIR_OP_INVOKE && copy.op != PIR_OP_INVOKE_INDIRECT) continue;

            bool is_ind     = (copy.op == PIR_OP_INVOKE_INDIRECT);
            bool is_nw      = is_ind ? ((mod->sigs.data[copy.sig_id].attrs & PIR_FATTR_NOUNWIND) != 0u)
                                     : ((mod->funcs.data[copy.operands[0].u.sym.id].attrs & PIR_FATTR_NOUNWIND) != 0u);
            PirType ret_ty  = is_ind ? mod->sigs.data[copy.sig_id].ret_type
                                     : mod->funcs.data[copy.operands[0].u.sym.id].ret_type;
            bool need_ret   = !(ret_ty.kind == PIR_TYPE_SCALAR && ret_ty.u.scalar == PIR_SCALAR_VOID) &&
                              target_uses_kind(&copy.targets[0], PIR_OPER_INVOKE_RET);

            /* Remove the invoke terminator from block b. */
            fn->blocks.data[b].insts.len = last_idx;

            if (is_nw) {
                /* Direct downgrade to call / call_indirect + jmp .ok. */
                PirInst call = pir_make_inst(is_ind ? PIR_OP_CALL_INDIRECT : PIR_OP_CALL, ret_ty, ln, lc);
                call.flags         = PIR_FLAG_NOUNWIND;
                call.sig_id        = copy.sig_id;
                call.operand_count = copy.operand_count;
                call.operands      = copy.operands;
                PirValueId rv      = PIR_INVALID_ID;
                if (need_ret) {
                    rv = pir_fn_new_val(mod, fn, "inv_r", ret_ty, b, PIR_INVALID_ID, 0u);
                    call.results[0] = rv;
                }
                pir_fn_append_inst(fn, &fn->blocks.data[b].insts, call);

                PirInst jmp = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
                jmp.targets[0] = clone_target_replace(mod, &copy.targets[0],
                                                      PIR_OPER_INVOKE_RET, rv, ret_ty);
                pir_fn_append_inst(fn, &fn->blocks.data[b].insts, jmp);
                continue;
            }

            /* Shadow-stack lowering: push frame, setjmp, branch to call vs catch. */
            PirBlockId call_blk  = add_block(mod, fn, "exc_call", ln);
            PirBlockId catch_blk = add_block(mod, fn, "exc_catch", ln);

            PirInstIdVec *cur = &fn->blocks.data[b].insts;

            PirInst ah = pir_make_inst(PIR_OP_ADDR, tptr, ln, lc);
            ah.operand_count = 1u; ah.operands = pir_alloc_ops(mod, 1u);
            ah.operands[0]   = make_global_sym(EXC_HEAD_NAME);
            ah.results[0]    = pir_fn_new_val(mod, fn, "eh", tptr, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, cur, ah);

            PirInst lprev = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
            lprev.align = pb; lprev.operand_count = 1u; lprev.operands = pir_alloc_ops(mod, 1u);
            lprev.operands[0] = pir_op_val(ah.results[0], tptr);
            lprev.results[0]  = pir_fn_new_val(mod, fn, "eprev", tptr, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, cur, lprev);

            PirInst sprev = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
            sprev.align = pb; sprev.operand_count = 2u; sprev.operands = pir_alloc_ops(mod, 2u);
            sprev.operands[0] = pir_op_val(lprev.results[0], tptr);
            sprev.operands[1] = pir_op_val(exc_frame, tptr);
            pir_fn_append_inst(fn, cur, sprev);

            PirInst push = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
            push.align = pb; push.operand_count = 2u; push.operands = pir_alloc_ops(mod, 2u);
            push.operands[0] = pir_op_val(exc_frame, tptr);
            push.operands[1] = pir_op_val(ah.results[0], tptr);
            pir_fn_append_inst(fn, cur, push);

            PirInst jb = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
            jb.flags = PIR_FLAG_INBOUNDS; jb.operand_count = 2u; jb.operands = pir_alloc_ops(mod, 2u);
            jb.operands[0] = pir_op_val(exc_frame, tptr);
            jb.operands[1] = pir_op_imm_i(2u * pb, false, idx_ty);
            jb.results[0]  = pir_fn_new_val(mod, fn, "ejb", tptr, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, cur, jb);

            PirInst sj = pir_make_inst(PIR_OP_CALL, ti32, ln, lc);
            sj.flags = PIR_FLAG_NOUNWIND; sj.operand_count = 2u; sj.operands = pir_alloc_ops(mod, 2u);
            sj.operands[0] = pir_op_sym_func(mod->funcs.data[setjmp_id].name, setjmp_id);
            sj.operands[1] = pir_op_val(jb.results[0], tptr);
            sj.results[0]  = pir_fn_new_val(mod, fn, "esj", ti32, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, cur, sj);

            PirInst cmp = pir_make_inst(PIR_OP_CMP_NE, ti32, ln, lc);
            cmp.operand_count = 2u; cmp.operands = pir_alloc_ops(mod, 2u);
            cmp.operands[0] = pir_op_val(sj.results[0], ti32);
            cmp.operands[1] = pir_op_imm_i(0u, false, ti32);
            cmp.results[0]  = pir_fn_new_val(mod, fn, "ec", ti8, b, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, cur, cmp);

            PirInst br = pir_make_inst(PIR_OP_BR, tvoid, ln, lc);
            br.operand_count = 1u; br.operands = pir_alloc_ops(mod, 1u);
            br.operands[0] = pir_op_val(cmp.results[0], ti8);
            br.targets[0].target = catch_blk;
            br.targets[1].target = call_blk;
            pir_fn_append_inst(fn, cur, br);

            /* .exc_call: call callee, pop frame, jmp .ok(...) */
            PirInstIdVec *cb = &fn->blocks.data[call_blk].insts;
            PirInst call = pir_make_inst(is_ind ? PIR_OP_CALL_INDIRECT : PIR_OP_CALL, ret_ty, ln, lc);
            call.sig_id        = copy.sig_id;
            call.operand_count = copy.operand_count;
            call.operands      = copy.operands;
            PirValueId rv      = PIR_INVALID_ID;
            if (need_ret) {
                rv = pir_fn_new_val(mod, fn, "inv_r", ret_ty, call_blk, PIR_INVALID_ID, 0u);
                call.results[0] = rv;
            }
            pir_fn_append_inst(fn, cb, call);

            PirInst pop1 = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
            pop1.align = pb; pop1.operand_count = 2u; pop1.operands = pir_alloc_ops(mod, 2u);
            pop1.operands[0] = pir_op_val(lprev.results[0], tptr);
            pop1.operands[1] = pir_op_val(ah.results[0], tptr);
            pir_fn_append_inst(fn, cb, pop1);

            PirInst jok = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
            jok.targets[0] = clone_target_replace(mod, &copy.targets[0],
                                                  PIR_OPER_INVOKE_RET, rv, ret_ty);
            pir_fn_append_inst(fn, cb, jok);

            /* .exc_catch: pop frame, load frame.exn, jmp .catch(...) */
            PirInstIdVec *kb = &fn->blocks.data[catch_blk].insts;
            PirInst pop2 = pir_make_inst(PIR_OP_STORE, tptr, ln, lc);
            pop2.align = pb; pop2.operand_count = 2u; pop2.operands = pir_alloc_ops(mod, 2u);
            pop2.operands[0] = pir_op_val(lprev.results[0], tptr);
            pop2.operands[1] = pir_op_val(ah.results[0], tptr);
            pir_fn_append_inst(fn, kb, pop2);

            PirInst ep = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, ln, lc);
            ep.flags = PIR_FLAG_INBOUNDS; ep.operand_count = 2u; ep.operands = pir_alloc_ops(mod, 2u);
            ep.operands[0] = pir_op_val(exc_frame, tptr);
            ep.operands[1] = pir_op_imm_i(pb, false, idx_ty);
            ep.results[0]  = pir_fn_new_val(mod, fn, "eexnp", tptr, catch_blk, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, kb, ep);

            PirInst lexn = pir_make_inst(PIR_OP_LOAD, tptr, ln, lc);
            lexn.align = pb; lexn.operand_count = 1u; lexn.operands = pir_alloc_ops(mod, 1u);
            lexn.operands[0] = pir_op_val(ep.results[0], tptr);
            lexn.results[0]  = pir_fn_new_val(mod, fn, "eexn", tptr, catch_blk, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, kb, lexn);

            PirInst jcat = pir_make_inst(PIR_OP_JMP, tvoid, ln, lc);
            jcat.targets[0] = clone_target_replace(mod, &copy.targets[1],
                                                   PIR_OPER_INVOKE_EXN, lexn.results[0], tptr);
            pir_fn_append_inst(fn, kb, jcat);
        }
    }
    return true;
}

bool pir_lower_exceptions(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    PirExceptionLowerer lowerer = lower_exceptions_shadow_stack;
    return lowerer(mod, target, diag);
}
