/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_ANALYSIS_LOOPS_H
#define PIR_ANALYSIS_LOOPS_H

#include <stdbool.h>
#include <stdint.h>

#include "analysis/cfg.h"
#include "analysis/dom.h"
#include "ir/ir.h"

typedef struct {
    PirBlockId    header;
    PirBlockId    preheader; /* PIR_INVALID_ID until ensured */
    PirBlockIdVec blocks;    /* All blocks belonging to the natural loop */
    bool         *in_loop;   /* Membership bitmap indexed by BlockId */
    uint32_t      num_blocks;
} PirLoop;

typedef struct {
    PIR_VEC(PirLoop) loops;
} PirLoopInfo;

void pir_loops_build(PirLoopInfo *li, const PirCfg *cfg, const PirDom *dom);
void pir_loops_destroy(PirLoopInfo *li);

/* Find existing single preheader or insert a dedicated preheader block before loop->header. */
PirBlockId pir_loop_ensure_preheader(PirModule *mod, PirFunction *fn,
                                     const PirCfg *cfg, PirLoop *loop);

#endif /* PIR_ANALYSIS_LOOPS_H */
