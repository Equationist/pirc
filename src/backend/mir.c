/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/mir.h"

#include <stdlib.h>
#include <string.h>

void mfunc_init(MFunc *mf, const PirFunction *pir_fn, uint32_t fn_index,
                const PirTargetSpec *target) {
    memset(mf, 0, sizeof(*mf));
    pir_arena_init(&mf->arena, 32768u);
    mf->pir_fn   = pir_fn;
    mf->fn_index = fn_index;
    mf->target   = target;
    mf->is_variadic = pir_fn ? pir_fn->is_variadic : false;
}

void mfunc_destroy(MFunc *mf) {
    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        PIR_VEC_FREE(&mf->blocks.data[b].insts);
        PIR_VEC_FREE(&mf->blocks.data[b].succs);
        PIR_VEC_FREE(&mf->blocks.data[b].preds);
    }
    PIR_VEC_FREE(&mf->vregs);
    PIR_VEC_FREE(&mf->blocks);
    PIR_VEC_FREE(&mf->slots);
    PIR_VEC_FREE(&mf->pcopies);
    PIR_VEC_FREE(&mf->jtables);
    pir_arena_destroy(&mf->arena);
}

MVReg mfunc_new_vreg(MFunc *mf, MRegClass rc, uint8_t byte_size) {
    MVReg id = mf->vregs.len;
    MVRegInfo info;
    info.rclass        = rc;
    info.byte_size     = byte_size;
    info.fixed_preg    = MREG_NONE;
    info.hint_preg     = MREG_NONE;
    info.assigned_preg = MREG_NONE;
    info.spill_slot    = -1;
    PIR_VEC_PUSH(&mf->vregs, info);
    return id;
}

uint32_t mfunc_new_block(MFunc *mf, const char *label) {
    uint32_t id = mf->blocks.len;
    MBlock blk;
    memset(&blk, 0, sizeof(blk));
    blk.id    = id;
    blk.label = label ? pir_arena_strdup(&mf->arena, label) : NULL;
    PIR_VEC_PUSH(&mf->blocks, blk);
    return id;
}

uint32_t mfunc_add_slot(MFunc *mf, uint64_t size, uint32_t align, bool is_spill) {
    uint32_t id = mf->slots.len;
    MStackSlot s;
    s.size      = size > 0u ? size : 1u;
    s.align     = align > 0u ? align : 1u;
    s.fp_offset = 0;
    s.is_spill  = is_spill;
    PIR_VEC_PUSH(&mf->slots, s);
    return id;
}

uint32_t mfunc_add_pcopy(MFunc *mf, const MPCopyEntry *entries, uint32_t count) {
    uint32_t id = mf->pcopies.len;
    MPCopyBundle b;
    b.count = count;
    if (count > 0u) {
        b.entries = PIR_ARENA_NEW_ARRAY(&mf->arena, MPCopyEntry, count);
        memcpy(b.entries, entries, (size_t)count * sizeof(MPCopyEntry));
    } else {
        b.entries = NULL;
    }
    PIR_VEC_PUSH(&mf->pcopies, b);
    return id;
}

uint32_t mfunc_add_jtable(MFunc *mf, int64_t min_val, uint32_t num_entries,
                          const uint32_t *targets, uint32_t default_blk) {
    uint32_t id = mf->jtables.len;
    MJTable jt;
    jt.min_val     = min_val;
    jt.num_entries = num_entries;
    jt.default_blk = default_blk;
    jt.targets     = PIR_ARENA_NEW_ARRAY(&mf->arena, uint32_t, num_entries);
    memcpy(jt.targets, targets, (size_t)num_entries * sizeof(uint32_t));
    PIR_VEC_PUSH(&mf->jtables, jt);
    return id;
}

static bool vec_contains_u32(const uint32_t *data, uint32_t len, uint32_t val) {
    for (uint32_t i = 0u; i < len; ++i) {
        if (data[i] == val) return true;
    }
    return false;
}

void mfunc_add_edge(MFunc *mf, uint32_t from_blk, uint32_t to_blk) {
    if (from_blk >= mf->blocks.len || to_blk >= mf->blocks.len) return;
    MBlock *fb = &mf->blocks.data[from_blk];
    MBlock *tb = &mf->blocks.data[to_blk];
    if (!vec_contains_u32(fb->succs.data, fb->succs.len, to_blk)) {
        PIR_VEC_PUSH(&fb->succs, to_blk);
    }
    if (!vec_contains_u32(tb->preds.data, tb->preds.len, from_blk)) {
        PIR_VEC_PUSH(&tb->preds, from_blk);
    }
}

void mblock_push(MBlock *blk, MInst inst) {
    PIR_VEC_PUSH(&blk->insts, inst);
}

MInst minst_make(MOpcode op, uint8_t width, uint32_t loc_idx) {
    MInst ins;
    memset(&ins, 0, sizeof(ins));
    ins.op      = op;
    ins.width   = width;
    ins.loc_idx = loc_idx;
    return ins;
}

MOperand mop_vreg_def(MVReg v) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind   = MOP_VREG;
    o.is_def = true;
    o.u.vreg = v;
    return o;
}

MOperand mop_vreg_use(MVReg v) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind   = MOP_VREG;
    o.is_use = true;
    o.u.vreg = v;
    return o;
}

MOperand mop_vreg_def_ec(MVReg v) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind             = MOP_VREG;
    o.is_def           = true;
    o.is_early_clobber = true;
    o.u.vreg           = v;
    return o;
}

MOperand mop_vreg_defuse(MVReg v) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind   = MOP_VREG;
    o.is_def = true;
    o.is_use = true;
    o.u.vreg = v;
    return o;
}

MOperand mop_preg_def(MPReg p) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind   = MOP_PREG;
    o.is_def = true;
    o.u.preg = p;
    return o;
}

MOperand mop_preg_use(MPReg p) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind   = MOP_PREG;
    o.is_use = true;
    o.u.preg = p;
    return o;
}

MOperand mop_preg_defuse(MPReg p) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind   = MOP_PREG;
    o.is_def = true;
    o.is_use = true;
    o.u.preg = p;
    return o;
}

MOperand mop_imm(int64_t val) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind  = MOP_IMM;
    o.u.imm = val;
    return o;
}

MOperand mop_sym(const char *name, int64_t off, bool is_func, bool is_tls, bool use_got) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind          = MOP_SYM;
    o.u.sym.name    = name;
    o.u.sym.offset  = off;
    o.u.sym.is_func = is_func;
    o.u.sym.is_tls  = is_tls;
    o.u.sym.use_got = use_got;
    return o;
}

MOperand mop_block(uint32_t blk_id) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind       = MOP_BLOCK;
    o.u.block_id = blk_id;
    return o;
}

MOperand mop_slot(uint32_t slot_id) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind      = MOP_SLOT;
    o.u.slot_id = slot_id;
    return o;
}

MOperand mop_jtable(uint32_t jt_id) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind        = MOP_JTABLE;
    o.u.jtable_id = jt_id;
    return o;
}

MOperand mop_cond(MCond c) {
    MOperand o;
    memset(&o, 0, sizeof(o));
    o.kind   = MOP_COND;
    o.u.cond = c;
    return o;
}
