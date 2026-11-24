/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_ANALYSIS_DOM_H
#define PIR_ANALYSIS_DOM_H

#include <stdbool.h>
#include <stdint.h>

#include "analysis/cfg.h"

typedef struct {
    uint32_t       num_blocks;
    PirBlockId    *idom;     /* Immediate dominator; PIR_INVALID_ID if unreachable */
    PirBlockIdVec *children; /* Dominator tree children per block */
} PirDom;

void pir_dom_build(PirDom *dom, const PirCfg *cfg);
void pir_dom_destroy(PirDom *dom);
bool pir_dom_dominates(const PirDom *dom, PirBlockId a, PirBlockId b);

/* Compute dominance frontiers df_out[0..cfg->num_blocks-1] (caller allocates & frees vecs). */
void pir_dom_compute_frontiers(const PirCfg *cfg, const PirDom *dom, PirBlockIdVec *df_out);

#endif /* PIR_ANALYSIS_DOM_H */
