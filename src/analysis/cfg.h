/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_ANALYSIS_CFG_H
#define PIR_ANALYSIS_CFG_H

#include <stdbool.h>
#include <stdint.h>

#include "ir/ir.h"

typedef struct {
    uint32_t       num_blocks;
    PirBlockIdVec *preds;
    PirBlockIdVec *succs;
    bool          *reachable;
    PirBlockId    *rpo;       /* Reverse post-order of reachable blocks */
    uint32_t       rpo_len;
    uint32_t      *rpo_num;   /* BlockId -> RPO index (PIR_INVALID_ID if unreachable) */
} PirCfg;

void pir_cfg_build(PirCfg *cfg, const PirFunction *fn);
void pir_cfg_destroy(PirCfg *cfg);

#endif /* PIR_ANALYSIS_CFG_H */
