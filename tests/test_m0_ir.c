/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ir/ir.h"

/* M0 smoke test: construct an in-memory HIR module matching PIR v0.3.0
 * and exercise arena allocation, dense handles, and clean teardown (ASan/UBSan). */
int main(void) {
    PirModule mod;
    pir_module_init(&mod);
    mod.target_triple = pir_arena_strdup(&mod.arena, "x86_64-unknown-linux-gnu");

    /* type @Point = { i32, i32 } */
    PirAggDef pt;
    memset(&pt, 0, sizeof(pt));
    pt.name        = pir_arena_strdup(&mod.arena, "Point");
    pt.kind        = PIR_AGG_STRUCT;
    pt.field_count = 2u;
    pt.fields      = PIR_ARENA_NEW_ARRAY(&mod.arena, PirType, 2u);
    pt.fields[0]   = pir_type_scalar(PIR_SCALAR_I32);
    pt.fields[1]   = pir_type_scalar(PIR_SCALAR_I32);
    PIR_VEC_PUSH(&mod.agg_types, pt);

    /* export func @add_i32(%a: i32, %b: i32) -> i32 cc "c" */
    PirFunction fn;
    memset(&fn, 0, sizeof(fn));
    fn.name        = pir_arena_strdup(&mod.arena, "add_i32");
    fn.linkage     = PIR_LINK_EXPORT;
    fn.cc          = PIR_CC_C;
    fn.attrs       = PIR_FATTR_NOUNWIND;
    fn.param_count = 2u;
    fn.params      = PIR_ARENA_NEW_ARRAY(&mod.arena, PirParamDecl, 2u);
    fn.params[0].name = pir_arena_strdup(&mod.arena, "a");
    fn.params[0].type = pir_type_scalar(PIR_SCALAR_I32);
    fn.params[1].name = pir_arena_strdup(&mod.arena, "b");
    fn.params[1].type = pir_type_scalar(PIR_SCALAR_I32);
    fn.ret_type    = pir_type_scalar(PIR_SCALAR_I32);
    fn.is_defined  = true;

    PirBlock entry;
    memset(&entry, 0, sizeof(entry));
    entry.label = pir_arena_strdup(&mod.arena, "entry");
    PIR_VEC_PUSH(&fn.blocks, entry);

    /* Define %a (id 0), %b (id 1), %sum (id 2) */
    PirValueDef va = { fn.params[0].name, fn.params[0].type, 0u, PIR_INVALID_ID, 0u };
    PirValueDef vb = { fn.params[1].name, fn.params[1].type, 0u, PIR_INVALID_ID, 1u };
    PIR_VEC_PUSH(&fn.values, va);
    PIR_VEC_PUSH(&fn.values, vb);
    PIR_VEC_PUSH(&fn.blocks.data[0].params, 0u);
    PIR_VEC_PUSH(&fn.blocks.data[0].params, 1u);

    /* %sum = add.i32 %a, %b [wrap] */
    PirValueDef vsum = { pir_arena_strdup(&mod.arena, "sum"),
                         pir_type_scalar(PIR_SCALAR_I32), 0u, 0u, 0u };
    PIR_VEC_PUSH(&fn.values, vsum);

    PirInst add_inst;
    memset(&add_inst, 0, sizeof(add_inst));
    add_inst.op            = PIR_OP_ADD;
    add_inst.ty            = pir_type_scalar(PIR_SCALAR_I32);
    add_inst.flags         = PIR_FLAG_WRAP;
    add_inst.operand_count = 2u;
    add_inst.operands      = PIR_ARENA_NEW_ARRAY(&mod.arena, PirOperand, 2u);
    add_inst.operands[0].kind  = PIR_OPER_VAL;
    add_inst.operands[0].type  = pir_type_scalar(PIR_SCALAR_I32);
    add_inst.operands[0].u.val = 0u;
    add_inst.operands[1].kind  = PIR_OPER_VAL;
    add_inst.operands[1].type  = pir_type_scalar(PIR_SCALAR_I32);
    add_inst.operands[1].u.val = 1u;
    add_inst.results[0]    = 2u;
    add_inst.results[1]    = PIR_INVALID_ID;
    PIR_VEC_PUSH(&fn.insts, add_inst);
    PIR_VEC_PUSH(&fn.blocks.data[0].insts, 0u);

    /* ret %sum */
    PirInst ret_inst;
    memset(&ret_inst, 0, sizeof(ret_inst));
    ret_inst.op            = PIR_OP_RET;
    ret_inst.ty            = pir_type_scalar(PIR_SCALAR_I32);
    ret_inst.operand_count = 1u;
    ret_inst.operands      = PIR_ARENA_NEW_ARRAY(&mod.arena, PirOperand, 1u);
    ret_inst.operands[0].kind  = PIR_OPER_VAL;
    ret_inst.operands[0].type  = pir_type_scalar(PIR_SCALAR_I32);
    ret_inst.operands[0].u.val = 2u;
    ret_inst.results[0]    = PIR_INVALID_ID;
    ret_inst.results[1]    = PIR_INVALID_ID;
    PIR_VEC_PUSH(&fn.insts, ret_inst);
    PIR_VEC_PUSH(&fn.blocks.data[0].insts, 1u);

    PIR_VEC_PUSH(&mod.funcs, fn);

    assert(mod.agg_types.len == 1u);
    assert(mod.funcs.len == 1u);
    assert(mod.funcs.data[0].blocks.data[0].insts.len == 2u);
    assert(mod.funcs.data[0].insts.data[0].op == PIR_OP_ADD);
    assert(strcmp(mod.target_triple, "x86_64-unknown-linux-gnu") == 0);

    pir_module_destroy(&mod);
    printf("PASS: test_m0_ir\n");
    return 0;
}
