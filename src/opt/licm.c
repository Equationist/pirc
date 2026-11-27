/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "opt/opt.h"

#include <stdlib.h>
#include <string.h>

#include "analysis/alias.h"
#include "analysis/cfg.h"
#include "analysis/dom.h"
#include "analysis/loops.h"
#include "lower/lower.h"

static bool op_is_invariant(const PirFunction *fn, const PirLoop *lp, PirOperand op) {
    if (op.kind != PIR_OPER_VAL) return true;
    if (op.u.val >= fn->values.len) return false;
    PirBlockId db = fn->values.data[op.u.val].def_block;
    if (db == PIR_INVALID_ID) return true; /* Function parameter */
    if (db >= lp->num_blocks) return true;
    return !lp->in_loop[db];
}

static bool all_ops_invariant(const PirFunction *fn, const PirLoop *lp, const PirInst *ins) {
    for (uint32_t i = 0u; i < ins->operand_count; ++i) {
        if (!op_is_invariant(fn, lp, ins->operands[i])) return false;
    }
    return true;
}

static uint32_t loop_scan_effects(const PirModule *mod, const PirFunction *fn, const PirLoop *lp) {
    uint32_t eff = PIR_EFF_NONE;
    for (uint32_t bi = 0u; bi < lp->blocks.len; ++bi) {
        const PirBlock *blk = &fn->blocks.data[lp->blocks.data[bi]];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            eff |= pir_inst_effects(mod, fn, &fn->insts.data[blk->insts.data[j]]);
        }
    }
    return eff;
}

static bool alloca_init_dominates(const PirFunction *fn, const PirDom *dom,
                                  PirBlockId target_blk, PirInstId alloca_id,
                                  uint64_t load_off, uint32_t load_sz, uint8_t ptr_bytes) {
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        if (b == target_blk || !pir_dom_dominates(dom, b, target_blk)) continue;
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *st = &fn->insts.data[blk->insts.data[j]];
            if (st->op != PIR_OP_STORE || st->operand_count != 2u) continue;
            uint32_t ssz = pir_type_bits(st->ty, ptr_bytes) / 8u;
            if (ssz < load_sz) continue;
            PirPtrInfo spi = pir_alias_decompose(fn, st->operands[1], ptr_bytes);
            if (spi.kind == PIR_PTR_ALLOCA && spi.obj_id == alloca_id &&
                spi.has_offset && spi.offset >= 0 &&
                (uint64_t)spi.offset <= load_off &&
                load_off - (uint64_t)spi.offset <= (uint64_t)(ssz - load_sz)) {
                return true;
            }
        }
    }
    return false;
}

static bool load_is_safe_to_hoist(const PirModule *mod, const PirFunction *fn,
                                  const PirTargetSpec *target, const PirDom *dom,
                                  const PirLoop *lp, PirBlockId b, uint32_t inst_idx,
                                  const PirInst *ins, bool has_barrier) {
    if (ins->op != PIR_OP_LOAD || (ins->flags & PIR_FLAG_VOLATILE) || has_barrier) return false;
    if (!op_is_invariant(fn, lp, ins->operands[0])) return false;

    uint32_t lsz = pir_type_bits(ins->ty, target->ptr_bytes) / 8u;
    if (lsz == 0u) return false;

    /* Verify no store inside the loop may alias this load. */
    for (uint32_t bi = 0u; bi < lp->blocks.len; ++bi) {
        const PirBlock *blk = &fn->blocks.data[lp->blocks.data[bi]];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *st = &fn->insts.data[blk->insts.data[j]];
            if (st->op == PIR_OP_STORE) {
                uint32_t ssz = pir_type_bits(st->ty, target->ptr_bytes) / 8u;
                if (pir_alias_query(fn, ins->operands[0], lsz, st->operands[1], ssz, target->ptr_bytes) != PIR_ALIAS_NO) {
                    return false;
                }
            }
        }
    }

    /* Dereference safety: a header load is guaranteed to execute on loop entry only if
     * no preceding instruction in the header can trap, diverge, or exit. Otherwise, only
     * provably in-bounds loads from known stack allocas or defined globals are safe to
     * speculate; `restrict` (noalias) pointer parameters may be NULL or out-of-bounds. */
    if (b == lp->header) {
        const PirBlock *hblk = &fn->blocks.data[b];
        bool preceded_by_hazard = false;
        for (uint32_t k = 0u; k < inst_idx; ++k) {
            if ((pir_inst_effects(mod, fn, &fn->insts.data[hblk->insts.data[k]]) & PIR_EFF_MAY_TRAP) != 0u) {
                preceded_by_hazard = true;
                break;
            }
        }
        if (!preceded_by_hazard) return true;
    }
    PirPtrInfo pi = pir_alias_decompose(fn, ins->operands[0], target->ptr_bytes);
    if (pi.kind == PIR_PTR_GLOBAL && pi.has_offset && pi.offset >= 0 && pi.global_name) {
        for (uint32_t gi = 0u; gi < mod->globals.len; ++gi) {
            const PirGlobal *g = &mod->globals.data[gi];
            if (g->name && strcmp(g->name, pi.global_name) == 0) {
                if (g->has_type && g->linkage != PIR_LINK_EXTERN) {
                    PirTypeLayout gly = pir_type_layout(mod, target, g->type);
                    uint32_t ga = g->align ? g->align : gly.align;
                    if (gly.size > 0u &&
                        (uint64_t)pi.offset <= gly.size &&
                        (uint64_t)lsz <= gly.size - (uint64_t)pi.offset &&
                        (ins->align <= 1u ||
                         (ga >= ins->align && ((uint64_t)pi.offset % ins->align) == 0u))) {
                        return true;
                    }
                }
                break;
            }
        }
    }
    if (pi.kind == PIR_PTR_ALLOCA && pi.has_offset && pi.offset >= 0 &&
        pi.obj_id < fn->insts.len && fn->insts.data[pi.obj_id].op == PIR_OP_ALLOCA_PTR) {
        const PirInst *al = &fn->insts.data[pi.obj_id];
        if ((uint64_t)pi.offset <= al->imm_u64 &&
            (uint64_t)lsz <= al->imm_u64 - (uint64_t)pi.offset &&
            (ins->align <= 1u ||
             (al->align >= ins->align && ((uint64_t)pi.offset % ins->align) == 0u)) &&
            alloca_init_dominates(fn, dom, lp->header, pi.obj_id,
                                  (uint64_t)pi.offset, lsz, target->ptr_bytes)) {
            return true;
        }
    }
    return false;
}

static void hoist_inst(PirFunction *fn, PirBlockId from_b, uint32_t idx, PirBlockId pre) {
    PirInstIdVec *src = &fn->blocks.data[from_b].insts;
    PirInstId iid = src->data[idx];
    for (uint32_t k = idx; k + 1u < src->len; ++k) {
        src->data[k] = src->data[k + 1u];
    }
    src->len--;

    /* Insert before preheader's terminator. */
    PirInstIdVec *dst = &fn->blocks.data[pre].insts;
    PIR_VEC_PUSH(dst, iid);
    if (dst->len >= 2u) {
        PirInstId term = dst->data[dst->len - 2u];
        dst->data[dst->len - 2u] = iid;
        dst->data[dst->len - 1u] = term;
    }

    PirInst *ins = &fn->insts.data[iid];
    if ((ins->op == PIR_OP_ADD || ins->op == PIR_OP_SUB || ins->op == PIR_OP_MUL) &&
        (ins->flags & (PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) != 0u) {
        ins->flags &= ~(uint32_t)(PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U);
        ins->flags |= PIR_FLAG_WRAP;
    }
    if (ins->op == PIR_OP_PTR_ADD) {
        ins->flags &= ~(uint32_t)PIR_FLAG_INBOUNDS;
    }
    if (ins->results[0] != PIR_INVALID_ID) fn->values.data[ins->results[0]].def_block = pre;
    if (ins->results[1] != PIR_INVALID_ID) fn->values.data[ins->results[1]].def_block = pre;
}

void pir_opt_licm(PirModule *mod, PirFunction *fn, const PirTargetSpec *target) {
    if (fn->blocks.len == 0u) return;

    for (uint32_t round = 0u; round < 64u; ++round) {
        PirCfg cfg;
        PirDom dom;
        PirLoopInfo li;
        pir_cfg_build(&cfg, fn);
        pir_dom_build(&dom, &cfg);
        pir_loops_build(&li, &cfg, &dom);

        bool rebuilt_needed = false;
        bool hoisted_any    = false;

        /* Sort loop indices innermost-first (smallest block count first). */
        uint32_t nl = li.loops.len;
        uint32_t *order = nl ? (uint32_t *)malloc(nl * sizeof(uint32_t)) : NULL;
        if (nl && !order) abort();
        for (uint32_t i = 0u; i < nl; ++i) order[i] = i;
        for (uint32_t i = 0u; i < nl; ++i) {
            for (uint32_t j = i + 1u; j < nl; ++j) {
                if (li.loops.data[order[j]].blocks.len < li.loops.data[order[i]].blocks.len) {
                    uint32_t tmp = order[i];
                    order[i] = order[j];
                    order[j] = tmp;
                }
            }
        }

        for (uint32_t oi = 0u; oi < nl && !rebuilt_needed; ++oi) {
            PirLoop *lp = &li.loops.data[order[oi]];
            uint32_t leff    = loop_scan_effects(mod, fn, lp);
            bool has_barrier = (leff & PIR_EFF_MEM_BARRIER) != 0u;
            bool has_call    = (leff & PIR_EFF_REAL_CALL) != 0u;

            bool loop_changed = true;
            while (loop_changed) {
                loop_changed = false;
                for (uint32_t ri = 0u; ri < cfg.rpo_len; ++ri) {
                    PirBlockId b = cfg.rpo[ri];
                    if (b >= lp->num_blocks || !lp->in_loop[b]) continue;

                    uint32_t j = 0u;
                    while (j + 1u < fn->blocks.data[b].insts.len) {
                        PirInstId iid = fn->blocks.data[b].insts.data[j];
                        const PirInst *ins = &fn->insts.data[iid];
                        bool can_hoist = false;

                        if ((pir_inst_effects(mod, fn, ins) & PIR_EFF_PURE_HOISTABLE) != 0u &&
                            all_ops_invariant(fn, lp, ins)) {
                            /* 128-bit vector regs are caller-saved on AAPCS64 and SysV x64, and
                             * trivial constants are cheaper to rematerialize than to hold in a
                             * callee-saved register or spill slot across a call. */
                            if (!(has_call && (ins->ty.kind == PIR_TYPE_VECTOR || ins->op == PIR_OP_CONST))) {
                                can_hoist = true;
                            }
                        } else if (load_is_safe_to_hoist(mod, fn, target, &dom, lp, b, j, ins, has_barrier)) {
                            can_hoist = true;
                        }

                        if (can_hoist) {
                            uint32_t prev_nblks = fn->blocks.len;
                            PirBlockId pre = pir_loop_ensure_preheader(mod, fn, &cfg, lp);
                            if (pre == PIR_INVALID_ID) {
                                j++;
                                continue;
                            }
                            hoist_inst(fn, b, j, pre);
                            hoisted_any  = true;
                            loop_changed = true;
                            if (fn->blocks.len != prev_nblks) {
                                /* A new preheader block was inserted; rebuild CFG/dom/loops. */
                                rebuilt_needed = true;
                                break;
                            }
                            continue;
                        }
                        j++;
                    }
                    if (rebuilt_needed) break;
                }
                if (rebuilt_needed) break;
            }
        }

        free(order);
        pir_loops_destroy(&li);
        pir_dom_destroy(&dom);
        pir_cfg_destroy(&cfg);

        if (!rebuilt_needed && !hoisted_any) break;
    }
}
