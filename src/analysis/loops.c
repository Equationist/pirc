/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "analysis/loops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lower/lower.h"

static PirLoop *find_or_create_loop(PirLoopInfo *li, PirBlockId header, uint32_t nblks) {
    for (uint32_t i = 0u; i < li->loops.len; ++i) {
        if (li->loops.data[i].header == header) {
            return &li->loops.data[i];
        }
    }
    PirLoop lp;
    memset(&lp, 0, sizeof(lp));
    lp.header     = header;
    lp.preheader  = PIR_INVALID_ID;
    lp.num_blocks = nblks;
    lp.in_loop    = (bool *)calloc(nblks, sizeof(bool));
    if (!lp.in_loop) abort();
    lp.in_loop[header] = true;
    PIR_VEC_PUSH(&lp.blocks, header);
    uint32_t idx = li->loops.len;
    PIR_VEC_PUSH(&li->loops, lp);
    return &li->loops.data[idx];
}

void pir_loops_build(PirLoopInfo *li, const PirCfg *cfg, const PirDom *dom) {
    memset(li, 0, sizeof(*li));
    uint32_t n = cfg->num_blocks;
    if (n == 0u) return;

    PirBlockId *stack = (PirBlockId *)malloc(n * sizeof(PirBlockId));
    if (!stack) abort();

    for (uint32_t ri = 0u; ri < cfg->rpo_len; ++ri) {
        PirBlockId u = cfg->rpo[ri];
        for (uint32_t s = 0u; s < cfg->succs[u].len; ++s) {
            PirBlockId h = cfg->succs[u].data[s];
            if (!pir_dom_dominates(dom, h, u)) continue;

            /* Backedge u -> h: reverse-DFS from u to collect natural loop body. */
            PirLoop *lp = find_or_create_loop(li, h, n);
            uint32_t sp = 0u;
            if (!lp->in_loop[u]) {
                lp->in_loop[u] = true;
                PIR_VEC_PUSH(&lp->blocks, u);
                stack[sp++] = u;
            }
            while (sp > 0u) {
                PirBlockId cur = stack[--sp];
                for (uint32_t p = 0u; p < cfg->preds[cur].len; ++p) {
                    PirBlockId pred = cfg->preds[cur].data[p];
                    if (cfg->reachable[pred] && !lp->in_loop[pred]) {
                        lp->in_loop[pred] = true;
                        PIR_VEC_PUSH(&lp->blocks, pred);
                        stack[sp++] = pred;
                    }
                }
            }
        }
    }
    free(stack);
}

void pir_loops_destroy(PirLoopInfo *li) {
    for (uint32_t i = 0u; i < li->loops.len; ++i) {
        PIR_VEC_FREE(&li->loops.data[i].blocks);
        free(li->loops.data[i].in_loop);
    }
    PIR_VEC_FREE(&li->loops);
}

static void redirect_target(PirBranchTarget *bt, PirBlockId from_h, PirBlockId to_pre) {
    if (bt->target == from_h) {
        bt->target = to_pre;
    }
}

PirBlockId pir_loop_ensure_preheader(PirModule *mod, PirFunction *fn,
                                     const PirCfg *cfg, PirLoop *loop) {
    if (loop->preheader != PIR_INVALID_ID) {
        return loop->preheader;
    }
    PirBlockId h = loop->header;
    if (h == 0u) return PIR_INVALID_ID;

    PirBlockId single_out = PIR_INVALID_ID;
    uint32_t out_cnt = 0u;
    for (uint32_t i = 0u; i < cfg->preds[h].len; ++i) {
        PirBlockId p = cfg->preds[h].data[i];
        if (!cfg->reachable[p] || loop->in_loop[p]) continue;
        single_out = p;
        out_cnt++;
    }
    if (out_cnt == 0u) return PIR_INVALID_ID;

    /* Reuse existing outside predecessor if it has a single successor (h). */
    if (out_cnt == 1u && cfg->succs[single_out].len == 1u) {
        loop->preheader = single_out;
        return single_out;
    }

    /* Insert a dedicated preheader block forwarding header's block parameters. */
    PirBlockId pre = pir_fn_new_block(mod, fn, "pre", fn->blocks.data[h].src_line);

    uint32_t np = fn->blocks.data[h].params.len;
    PirInst jmp = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID),
                                fn->blocks.data[h].src_line, 0u);
    jmp.targets[0].target    = h;
    jmp.targets[0].arg_count = np;
    if (np > 0u) {
        jmp.targets[0].args = pir_alloc_ops(mod, np);
        for (uint32_t k = 0u; k < np; ++k) {
            PirType pty = fn->values.data[fn->blocks.data[h].params.data[k]].type;
            PirValueId pv = pir_fn_new_val(mod, fn, "ph", pty, pre, PIR_INVALID_ID, k);
            PIR_VEC_PUSH(&fn->blocks.data[pre].params, pv);
            jmp.targets[0].args[k] = pir_op_val(pv, pty);
        }
    }
    pir_fn_append_inst(fn, &fn->blocks.data[pre].insts, jmp);

    /* Redirect all outside predecessors of h to pre. */
    for (uint32_t i = 0u; i < cfg->preds[h].len; ++i) {
        PirBlockId p = cfg->preds[h].data[i];
        if (!cfg->reachable[p] || loop->in_loop[p]) continue;
        PirBlock *pb = &fn->blocks.data[p];
        if (pb->insts.len == 0u) continue;
        PirInst *term = &fn->insts.data[pb->insts.data[pb->insts.len - 1u]];
        if (term->op == PIR_OP_JMP || term->op == PIR_OP_BR ||
            term->op == PIR_OP_INVOKE || term->op == PIR_OP_INVOKE_INDIRECT ||
            term->op == PIR_OP_SWITCH) {
            redirect_target(&term->targets[0], h, pre);
            if (term->op == PIR_OP_BR || term->op == PIR_OP_INVOKE ||
                term->op == PIR_OP_INVOKE_INDIRECT) {
                redirect_target(&term->targets[1], h, pre);
            }
            if (term->op == PIR_OP_SWITCH) {
                for (uint32_t c = 0u; c < term->switch_case_count; ++c) {
                    redirect_target(&term->switch_cases[c].target, h, pre);
                }
            }
        }
    }

    loop->preheader = pre;
    return pre;
}
