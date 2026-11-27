/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "analysis/dom.h"

#include <stdlib.h>
#include <string.h>

static PirBlockId intersect(const PirDom *dom, const PirCfg *cfg, PirBlockId b1, PirBlockId b2) {
    while (b1 != b2) {
        while (cfg->rpo_num[b1] > cfg->rpo_num[b2]) {
            b1 = dom->idom[b1];
        }
        while (cfg->rpo_num[b2] > cfg->rpo_num[b1]) {
            b2 = dom->idom[b2];
        }
    }
    return b1;
}

void pir_dom_build(PirDom *dom, const PirCfg *cfg) {
    memset(dom, 0, sizeof(*dom));
    dom->num_blocks = cfg->num_blocks;
    if (cfg->num_blocks == 0u) {
        return;
    }
    dom->idom     = (PirBlockId *)malloc(cfg->num_blocks * sizeof(PirBlockId));
    dom->children = (PirBlockIdVec *)calloc(cfg->num_blocks, sizeof(PirBlockIdVec));
    dom->tin      = (uint32_t *)calloc(cfg->num_blocks, sizeof(uint32_t));
    dom->tout     = (uint32_t *)calloc(cfg->num_blocks, sizeof(uint32_t));
    if (!dom->idom || !dom->children || !dom->tin || !dom->tout) {
        abort();
    }
    for (uint32_t i = 0u; i < cfg->num_blocks; ++i) {
        dom->idom[i] = PIR_INVALID_ID;
    }
    dom->idom[0] = 0u;

    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t ri = 1u; ri < cfg->rpo_len; ++ri) {
            PirBlockId b = cfg->rpo[ri];
            PirBlockId new_idom = PIR_INVALID_ID;
            const PirBlockIdVec *pv = &cfg->preds[b];
            for (uint32_t i = 0u; i < pv->len; ++i) {
                PirBlockId p = pv->data[i];
                if (dom->idom[p] == PIR_INVALID_ID) {
                    continue;
                }
                if (new_idom == PIR_INVALID_ID) {
                    new_idom = p;
                } else {
                    new_idom = intersect(dom, cfg, p, new_idom);
                }
            }
            if (new_idom != PIR_INVALID_ID && dom->idom[b] != new_idom) {
                dom->idom[b] = new_idom;
                changed = true;
            }
        }
    }

    for (uint32_t ri = 1u; ri < cfg->rpo_len; ++ri) {
        PirBlockId b = cfg->rpo[ri];
        PirBlockId p = dom->idom[b];
        if (p != PIR_INVALID_ID && p != b) {
            PIR_VEC_PUSH(&dom->children[p], b);
        }
    }

    /* Compute Euler-tour timestamps iteratively for O(1) dominance queries. */
    typedef struct { PirBlockId b; uint32_t ci; } DomDfsFrame;
    PIR_VEC(DomDfsFrame) stk = {0};
    uint32_t timer = 0u;
    dom->tin[0] = ++timer;
    DomDfsFrame root_fr = { 0u, 0u };
    PIR_VEC_PUSH(&stk, root_fr);
    while (stk.len > 0u) {
        DomDfsFrame *fr = &stk.data[stk.len - 1u];
        PirBlockId cur = fr->b;
        if (fr->ci < dom->children[cur].len) {
            PirBlockId ch = dom->children[cur].data[fr->ci++];
            dom->tin[ch] = ++timer;
            DomDfsFrame next_fr = { ch, 0u };
            PIR_VEC_PUSH(&stk, next_fr);
        } else {
            dom->tout[cur] = timer;
            stk.len--;
        }
    }
    PIR_VEC_FREE(&stk);
}

void pir_dom_destroy(PirDom *dom) {
    for (uint32_t i = 0u; i < dom->num_blocks; ++i) {
        PIR_VEC_FREE(&dom->children[i]);
    }
    free(dom->children);
    free(dom->idom);
    free(dom->tin);
    free(dom->tout);
    memset(dom, 0, sizeof(*dom));
}

bool pir_dom_dominates(const PirDom *dom, PirBlockId a, PirBlockId b) {
    if (a >= dom->num_blocks || b >= dom->num_blocks) {
        return false;
    }
    uint32_t ta = dom->tin[a];
    uint32_t tb = dom->tin[b];
    if (ta == 0u || tb == 0u) {
        return false;
    }
    return ta <= tb && tb <= dom->tout[a];
}

static void add_unique_blk(PirBlockIdVec *v, PirBlockId b) {
    for (uint32_t i = 0u; i < v->len; ++i) {
        if (v->data[i] == b) return;
    }
    PIR_VEC_PUSH(v, b);
}

void pir_dom_compute_frontiers(const PirCfg *cfg, const PirDom *dom, PirBlockIdVec *df_out) {
    for (uint32_t ri = 0u; ri < cfg->rpo_len; ++ri) {
        PirBlockId b = cfg->rpo[ri];
        if (cfg->preds[b].len < 2u) continue;
        for (uint32_t i = 0u; i < cfg->preds[b].len; ++i) {
            PirBlockId runner = cfg->preds[b].data[i];
            if (!cfg->reachable[runner]) continue;
            while (runner != dom->idom[b] && runner != PIR_INVALID_ID) {
                add_unique_blk(&df_out[runner], b);
                if (runner == 0u) break;
                runner = dom->idom[runner];
            }
        }
    }
}
