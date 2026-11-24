/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "analysis/cfg.h"

#include <stdlib.h>
#include <string.h>

static void add_unique_edge(PirCfg *cfg, PirBlockId from, PirBlockId to) {
    if (from >= cfg->num_blocks || to >= cfg->num_blocks) {
        return;
    }
    PirBlockIdVec *sv = &cfg->succs[from];
    for (uint32_t i = 0u; i < sv->len; ++i) {
        if (sv->data[i] == to) {
            return;
        }
    }
    PIR_VEC_PUSH(sv, to);
    PIR_VEC_PUSH(&cfg->preds[to], from);
}

void pir_cfg_build(PirCfg *cfg, const PirFunction *fn) {
    memset(cfg, 0, sizeof(*cfg));
    uint32_t n = fn->blocks.len;
    cfg->num_blocks = n;
    if (n == 0u) {
        return;
    }

    cfg->preds     = (PirBlockIdVec *)calloc(n, sizeof(PirBlockIdVec));
    cfg->succs     = (PirBlockIdVec *)calloc(n, sizeof(PirBlockIdVec));
    cfg->reachable = (bool *)calloc(n, sizeof(bool));
    cfg->rpo       = (PirBlockId *)malloc(n * sizeof(PirBlockId));
    cfg->rpo_num   = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!cfg->preds || !cfg->succs || !cfg->reachable || !cfg->rpo || !cfg->rpo_num) {
        abort();
    }
    for (uint32_t i = 0u; i < n; ++i) {
        cfg->rpo_num[i] = PIR_INVALID_ID;
    }

    /* Extract CFG edges from each block's terminator. */
    for (uint32_t b = 0u; b < n; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        if (blk->insts.len == 0u) {
            continue;
        }
        PirInstId term_id = blk->insts.data[blk->insts.len - 1u];
        const PirInst *inst = &fn->insts.data[term_id];
        switch (inst->op) {
            case PIR_OP_JMP:
                add_unique_edge(cfg, b, inst->targets[0].target);
                break;
            case PIR_OP_BR:
            case PIR_OP_INVOKE:
            case PIR_OP_INVOKE_INDIRECT:
                add_unique_edge(cfg, b, inst->targets[0].target);
                add_unique_edge(cfg, b, inst->targets[1].target);
                break;
            case PIR_OP_SWITCH:
                add_unique_edge(cfg, b, inst->targets[0].target);
                for (uint32_t k = 0u; k < inst->switch_case_count; ++k) {
                    add_unique_edge(cfg, b, inst->switch_cases[k].target.target);
                }
                break;
            case PIR_OP_RET:
            case PIR_OP_TRAP:
            case PIR_OP_UNREACHABLE:
            case PIR_OP_RESUME:
                break;
            case PIR_OP_CONST: case PIR_OP_ADDR:
            case PIR_OP_ADD: case PIR_OP_SUB: case PIR_OP_MUL:
            case PIR_OP_DIV_S: case PIR_OP_DIV_U: case PIR_OP_REM_S: case PIR_OP_REM_U:
            case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
            case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
            case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
            case PIR_OP_MUL_WIDE_U: case PIR_OP_MUL_WIDE_S:
            case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
            case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR:
            case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR:
            case PIR_OP_CLZ: case PIR_OP_CTZ: case PIR_OP_POPCNT: case PIR_OP_BSWAP:
            case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV: case PIR_OP_FNEG:
            case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
            case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_S: case PIR_OP_CMP_LE_U:
            case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_S: case PIR_OP_CMP_GE_U:
            case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
            case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
            case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
            case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE:
            case PIR_OP_SELECT:
            case PIR_OP_ZEXT: case PIR_OP_SEXT: case PIR_OP_TRUNC:
            case PIR_OP_FPTOSI: case PIR_OP_FPTOUI: case PIR_OP_SITOFP: case PIR_OP_UITOFP:
            case PIR_OP_FPEXT: case PIR_OP_FPTRUNC: case PIR_OP_BITCAST:
            case PIR_OP_PTRTOINT: case PIR_OP_INTTOPTR:
            case PIR_OP_SPLAT: case PIR_OP_EXTRACT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
            case PIR_OP_ALLOCA: case PIR_OP_ALLOCA_PTR: case PIR_OP_ALLOCA_DYN:
            case PIR_OP_STACKSAVE: case PIR_OP_STACKRESTORE:
            case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR: case PIR_OP_PTR_ADD:
            case PIR_OP_LOAD: case PIR_OP_STORE:
            case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
            case PIR_OP_ATOMIC_LOAD: case PIR_OP_ATOMIC_STORE:
            case PIR_OP_ATOMIC_RMW: case PIR_OP_ATOMIC_CMPXCHG: case PIR_OP_FENCE:
            case PIR_OP_VA_START: case PIR_OP_VA_ARG: case PIR_OP_VA_ARG_BUF:
            case PIR_OP_VA_COPY: case PIR_OP_VA_END:
            case PIR_OP_CALL: case PIR_OP_CALL_INDIRECT:
                break;
        }
    }

    /* Iterative post-order DFS from entry block 0. */
    PirBlockId *stack_blk = (PirBlockId *)malloc(n * sizeof(PirBlockId));
    uint32_t   *stack_idx = (uint32_t *)malloc(n * sizeof(uint32_t));
    PirBlockId *postorder = (PirBlockId *)malloc(n * sizeof(PirBlockId));
    if (!stack_blk || !stack_idx || !postorder) {
        abort();
    }
    uint32_t sp = 0u, po_len = 0u;
    cfg->reachable[0] = true;
    stack_blk[sp] = 0u;
    stack_idx[sp] = 0u;
    sp++;

    while (sp > 0u) {
        PirBlockId cur = stack_blk[sp - 1u];
        uint32_t   si  = stack_idx[sp - 1u];
        if (si < cfg->succs[cur].len) {
            stack_idx[sp - 1u] = si + 1u;
            PirBlockId nxt = cfg->succs[cur].data[si];
            if (!cfg->reachable[nxt]) {
                cfg->reachable[nxt] = true;
                stack_blk[sp] = nxt;
                stack_idx[sp] = 0u;
                sp++;
            }
        } else {
            sp--;
            postorder[po_len++] = cur;
        }
    }

    cfg->rpo_len = po_len;
    for (uint32_t i = 0u; i < po_len; ++i) {
        PirBlockId b = postorder[po_len - 1u - i];
        cfg->rpo[i]     = b;
        cfg->rpo_num[b] = i;
    }

    free(stack_blk);
    free(stack_idx);
    free(postorder);
}

void pir_cfg_destroy(PirCfg *cfg) {
    for (uint32_t i = 0u; i < cfg->num_blocks; ++i) {
        PIR_VEC_FREE(&cfg->preds[i]);
        PIR_VEC_FREE(&cfg->succs[i]);
    }
    free(cfg->preds);
    free(cfg->succs);
    free(cfg->reachable);
    free(cfg->rpo);
    free(cfg->rpo_num);
    memset(cfg, 0, sizeof(*cfg));
}
