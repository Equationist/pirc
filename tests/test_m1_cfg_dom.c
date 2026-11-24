/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "analysis/cfg.h"
#include "analysis/dom.h"
#include "common/diag.h"
#include "ir/ir.h"
#include "ir/parser.h"
#include "ir/verify.h"
#include "target/target.h"

int main(void) {
    const char *src =
        "target \"x86_64-unknown-linux-gnu\"\n"
        "func @cfg_test(%c: i8) -> i32 {\n"
        ".entry:\n"
        "    br %c, .left, .right\n"
        ".left:\n"
        "    jmp .merge\n"
        ".right:\n"
        "    jmp .merge\n"
        ".merge:\n"
        "    br %c, .loop, .exit\n"
        ".loop:\n"
        "    br %c, .loop, .exit\n"
        ".exit:\n"
        "    ret 0\n"
        ".dead:\n"
        "    unreachable\n"
        "}\n";

    PirModule mod;
    pir_module_init(&mod);
    PirDiag diag;
    pir_diag_init(&diag, "<test_m1_cfg_dom>");

    assert(pir_parse_module(src, strlen(src), "<test_m1_cfg_dom>", &mod, &diag));
    PirTargetSpec target;
    assert(pir_target_from_triple("x86_64-unknown-linux-gnu", &target));
    assert(pir_verify_module(&mod, PIR_VERIFY_LIR, &target, &diag));

    const PirFunction *fn = &mod.funcs.data[0];
    PirCfg cfg;
    PirDom dom;
    pir_cfg_build(&cfg, fn);
    pir_dom_build(&dom, &cfg);

    /* Blocks: 0=.entry, 1=.left, 2=.right, 3=.merge, 4=.loop, 5=.exit, 6=.dead */
    assert(cfg.num_blocks == 7u);
    assert(cfg.rpo_len == 6u);
    assert(cfg.reachable[0] && cfg.reachable[1] && cfg.reachable[2] &&
           cfg.reachable[3] && cfg.reachable[4] && cfg.reachable[5]);
    assert(!cfg.reachable[6]);

    assert(dom.idom[0] == 0u);
    assert(dom.idom[1] == 0u);
    assert(dom.idom[2] == 0u);
    assert(dom.idom[3] == 0u);  /* .entry immediately dominates .merge across diamond */
    assert(dom.idom[4] == 3u);  /* .merge dominates .loop */
    assert(dom.idom[5] == 3u);  /* .merge dominates .exit (reachable via .merge and .loop) */
    assert(dom.idom[6] == PIR_INVALID_ID);

    assert(pir_dom_dominates(&dom, 0u, 5u));
    assert(pir_dom_dominates(&dom, 3u, 5u));
    assert(!pir_dom_dominates(&dom, 1u, 3u));
    assert(!pir_dom_dominates(&dom, 4u, 5u));
    assert(!pir_dom_dominates(&dom, 0u, 6u));

    pir_dom_destroy(&dom);
    pir_cfg_destroy(&cfg);
    pir_module_destroy(&mod);
    printf("PASS: test_m1_cfg_dom\n");
    return 0;
}
