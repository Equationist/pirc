/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "opt/opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/alias.h"
#include "analysis/cfg.h"
#include "analysis/dom.h"
#include "analysis/loops.h"
#include "lower/lower.h"

/*
 * General LIR Inner-Loop Auto-Vectorizer (128-bit SIMD, 2x unrolled).
 *
 * Matches canonical 2-block innermost loops (header + body):
 *   header(%iv: i64|i32):
 *     %c = cmp_lt_s/u %iv, %limit  (or mirrored cmp_ge_s/u)
 *     br %c, .body, .exit
 *   body:
 *     unit-stride loads/stores of uniform 32-bit or 64-bit scalar elements
 *     lane-wise pure vectorizable arithmetic / bitwise / compare / select ops
 *     %iv1 = add %iv, 1
 *     jmp .header(%iv1)
 *
 * When alias analysis proves store bases do not alias any other load/store
 * base (except exact same-index read-modify-write), emits an overflow-safe
 * 2x-unrolled 128-bit vector loop (STEP = 2 * VF elements/iter) that falls
 * directly into the original scalar loop for the remaining 0..STEP-1 tail
 * elements (and for n < STEP or n <= iv0).
 */

typedef struct {
    bool       is_mem_addr;   /* addr calc instruction (shl/mul/ptr_add/sext/sub) */
    bool       is_iv_step;    /* %iv1 = add %iv, 1 */
    bool       is_vec_op;     /* lane-wise vectorizable op or load/store */
    PirOperand mem_base;      /* loop-invariant base ptr (for load/store) */
    int64_t    mem_delta;     /* constant element index offset (e.g. -1 for 1-based arrays) */
} BodyInstInfo;

typedef struct {
    PirOperand base;
    int64_t    delta;
} MemAccessDesc;

typedef struct {
    PirOperand a_base;
    int64_t    a_delta;
    PirOperand b_base;
    int64_t    b_delta;
} RuntimeAliasPair;

typedef struct {
    PirOperand scalar_op;
    PirType    vec_ty;
    PirValueId splat_val;
} SplatEntry;

static bool op_is_imm_u64(PirOperand op, uint64_t *out) {
    if (op.kind != PIR_OPER_IMM_INT || op.u.imm_int.is_negative) return false;
    *out = op.u.imm_int.lo;
    return true;
}

static bool lv_small_imm_to_i64(const PirOperand *c, int64_t *out) {
    if (c->u.imm_int.hi != 0u) return false;
    int64_t v;
    if (c->type.kind == PIR_TYPE_SCALAR && c->type.u.scalar == PIR_SCALAR_I32 &&
        !c->u.imm_int.is_negative) {
        v = (int64_t)(int32_t)(uint32_t)c->u.imm_int.lo;
    } else if (c->u.imm_int.is_negative) {
        if (c->u.imm_int.lo > 1048576u) return false;
        v = -(int64_t)c->u.imm_int.lo;
    } else {
        if (c->u.imm_int.lo > 1048576u) return false;
        v = (int64_t)c->u.imm_int.lo;
    }
    if (v < -1048576 || v > 1048576) return false;
    *out = v;
    return true;
}

static bool op_is_imm_i64(const PirFunction *fn, PirOperand op, int64_t *out) {
    if (op.kind == PIR_OPER_IMM_INT) {
        return lv_small_imm_to_i64(&op, out);
    }
    if (op.kind == PIR_OPER_VAL && op.u.val < fn->values.len) {
        PirInstId did = fn->values.data[op.u.val].def_inst;
        if (did != PIR_INVALID_ID && fn->insts.data[did].op == PIR_OP_CONST &&
            fn->insts.data[did].operand_count == 1u &&
            fn->insts.data[did].operands[0].kind == PIR_OPER_IMM_INT) {
            return lv_small_imm_to_i64(&fn->insts.data[did].operands[0], out);
        }
    }
    return false;
}

static bool operand_outside_loop(const PirFunction *fn, const PirLoop *loop, PirOperand op) {
    if (op.kind != PIR_OPER_VAL) return true;
    if (op.u.val >= fn->values.len) return false;
    PirBlockId db = fn->values.data[op.u.val].def_block;
    if (db == PIR_INVALID_ID) return true; /* function param */
    return db < loop->num_blocks && !loop->in_loop[db];
}

static bool is_vectorizable_elem_type(PirType ty, uint32_t *out_bytes, uint32_t *out_vf,
                                      PirVecType *out_vty) {
    if (ty.kind != PIR_TYPE_SCALAR) return false;
    switch (ty.u.scalar) {
        case PIR_SCALAR_I8:  *out_bytes = 1u; *out_vf = 16u; *out_vty = PIR_VEC_V16I8; return true;
        case PIR_SCALAR_I16: *out_bytes = 2u; *out_vf = 8u;  *out_vty = PIR_VEC_V8I16; return true;
        case PIR_SCALAR_F32: *out_bytes = 4u; *out_vf = 4u;  *out_vty = PIR_VEC_V4F32; return true;
        case PIR_SCALAR_I32: *out_bytes = 4u; *out_vf = 4u;  *out_vty = PIR_VEC_V4I32; return true;
        case PIR_SCALAR_F64: *out_bytes = 8u; *out_vf = 2u;  *out_vty = PIR_VEC_V2F64; return true;
        case PIR_SCALAR_I64: *out_bytes = 8u; *out_vf = 2u;  *out_vty = PIR_VEC_V2I64; return true;
        case PIR_SCALAR_VOID:
        case PIR_SCALAR_I128:
        case PIR_SCALAR_PTR:
        case PIR_SCALAR_VALIST:
            return false;
    }
    return false;
}

static PirVecType vec_type_for_scalar_lane(PirScalarType sc, uint32_t vf) {
    if (vf == 16u) return PIR_VEC_V16I8;
    if (vf == 8u)  return PIR_VEC_V8I16;
    if (vf == 4u) {
        return (sc == PIR_SCALAR_F32) ? PIR_VEC_V4F32 : PIR_VEC_V4I32;
    }
    return (sc == PIR_SCALAR_F64) ? PIR_VEC_V2F64 : PIR_VEC_V2I64;
}

/* Match `iv` or `add/sub iv, const_delta` (with optional sext/zext matching `is_signed`). */
static bool match_iv_plus_const(const PirFunction *fn, PirBlockId body_bid,
                                PirValueId iv, bool is_signed, PirOperand idx_op,
                                int64_t *out_delta, bool *out_narrow_shift,
                                PirInstId *out_ext, PirInstId *out_adj) {
    *out_delta        = 0;
    *out_narrow_shift = false;
    *out_ext          = PIR_INVALID_ID;
    *out_adj          = PIR_INVALID_ID;
    if (idx_op.kind != PIR_OPER_VAL) return false;
    PirValueId cur = idx_op.u.val;
    if (cur == iv) return true;

    bool saw_ext = false;
    /* Unwrap optional sext/zext of i32 IV (or i32 IV + const) to i64; signedness must match loop IV! */
    if (cur < fn->values.len && fn->values.data[cur].def_inst != PIR_INVALID_ID &&
        fn->values.data[cur].def_block == body_bid) {
        PirInstId xid = fn->values.data[cur].def_inst;
        const PirInst *xins = &fn->insts.data[xid];
        PirOpcode expect_ext = is_signed ? PIR_OP_SEXT : PIR_OP_ZEXT;
        if (xins->op == expect_ext &&
            xins->operand_count == 1u && xins->operands[0].kind == PIR_OPER_VAL) {
            *out_ext = xid;
            saw_ext  = true;
            cur = xins->operands[0].u.val;
            if (cur == iv) return true;
        }
    }

    /* Allow `add iv, C` or `sub iv, C` (e.g. Ada 1-based `sub %iv, 1`). */
    if (cur < fn->values.len && fn->values.data[cur].def_inst != PIR_INVALID_ID &&
        fn->values.data[cur].def_block == body_bid) {
        PirInstId aid = fn->values.data[cur].def_inst;
        const PirInst *ains = &fn->insts.data[aid];
        if ((ains->flags & (PIR_FLAG_TRAP | PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) == 0u && ains->operand_count == 2u) {
            int64_t c = 0;
            if (ains->op == PIR_OP_ADD) {
                if (ains->operands[0].kind == PIR_OPER_VAL && ains->operands[0].u.val == iv &&
                    op_is_imm_i64(fn, ains->operands[1], &c)) {
                    *out_delta        = c;
                    *out_narrow_shift = saw_ext;
                    *out_adj          = aid;
                    return true;
                }
                if (ains->operands[1].kind == PIR_OPER_VAL && ains->operands[1].u.val == iv &&
                    op_is_imm_i64(fn, ains->operands[0], &c)) {
                    *out_delta        = c;
                    *out_narrow_shift = saw_ext;
                    *out_adj          = aid;
                    return true;
                }
            } else if (ains->op == PIR_OP_SUB) {
                if (ains->operands[0].kind == PIR_OPER_VAL && ains->operands[0].u.val == iv &&
                    op_is_imm_i64(fn, ains->operands[1], &c)) {
                    *out_delta        = -c;
                    *out_narrow_shift = saw_ext;
                    *out_adj          = aid;
                    return true;
                }
            }
        }
    }
    return false;
}

/* Check if `off_op` equals `(iv + delta) * elem_bytes` (via `shl`/`mul`, or direct for 1-byte elem). */
static bool match_unit_stride_offset(const PirFunction *fn, PirBlockId body_bid,
                                     PirValueId iv, bool is_signed, uint32_t elem_bytes,
                                     PirOperand off_op, int64_t *out_delta,
                                     bool *out_narrow_shift,
                                     PirInstId *out_inst1, PirInstId *out_inst2,
                                     PirInstId *out_inst3) {
    *out_delta        = 0;
    *out_narrow_shift = false;
    *out_inst1        = PIR_INVALID_ID;
    *out_inst2        = PIR_INVALID_ID;
    *out_inst3        = PIR_INVALID_ID;

    if (elem_bytes == 1u) {
        if (match_iv_plus_const(fn, body_bid, iv, is_signed, off_op, out_delta, out_narrow_shift,
                                out_inst2, out_inst3)) {
            return true;
        }
        return false;
    }

    if (off_op.kind != PIR_OPER_VAL) return false;
    PirValueId ov = off_op.u.val;
    if (ov >= fn->values.len) return false;
    const PirValueDef *vd = &fn->values.data[ov];
    if (vd->def_inst == PIR_INVALID_ID || vd->def_block != body_bid) return false;
    const PirInst *ins = &fn->insts.data[vd->def_inst];
    if ((ins->flags & (PIR_FLAG_TRAP | PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u) return false;

    uint32_t shift_expect = (elem_bytes == 2u) ? 1u : (elem_bytes == 4u) ? 2u : 3u;
    PirOperand idx_op;
    bool matched = false;
    if (ins->op == PIR_OP_SHL && ins->operand_count == 2u) {
        uint64_t sh = 0u;
        if (op_is_imm_u64(ins->operands[1], &sh) && sh == (uint64_t)shift_expect) {
            idx_op = ins->operands[0];
            matched = true;
        }
    } else if (ins->op == PIR_OP_MUL && ins->operand_count == 2u) {
        uint64_t sc = 0u;
        if (op_is_imm_u64(ins->operands[1], &sc) && sc == (uint64_t)elem_bytes) {
            idx_op = ins->operands[0];
            matched = true;
        } else if (op_is_imm_u64(ins->operands[0], &sc) && sc == (uint64_t)elem_bytes) {
            idx_op = ins->operands[1];
            matched = true;
        }
    }
    if (!matched) return false;

    *out_inst1 = vd->def_inst;
    return match_iv_plus_const(fn, body_bid, iv, is_signed, idx_op, out_delta, out_narrow_shift,
                               out_inst2, out_inst3);
}

/* Match `ptr_add base, ((iv + delta) * elem_bytes)` where `base` is loop-invariant. */
static bool match_unit_stride_ptr(const PirFunction *fn, const PirLoop *loop, PirBlockId body_bid,
                                  PirValueId iv, bool is_signed, uint32_t elem_bytes, PirOperand ptr_op,
                                  PirOperand *out_base, int64_t *out_delta,
                                  bool *out_narrow_shift, PirInstId *out_pa,
                                  PirInstId *out_off1, PirInstId *out_off2,
                                  PirInstId *out_off3) {
    if (ptr_op.kind != PIR_OPER_VAL) return false;
    PirValueId pv = ptr_op.u.val;
    if (pv >= fn->values.len) return false;
    const PirValueDef *vd = &fn->values.data[pv];
    if (vd->def_inst == PIR_INVALID_ID || vd->def_block != body_bid) return false;
    const PirInst *pa = &fn->insts.data[vd->def_inst];
    if (pa->op != PIR_OP_PTR_ADD || pa->operand_count != 2u) return false;
    /* ptr_add.i32 sign-extends its i32 offset; reject for unsigned IV loops. */
    if (pa->ty.kind == PIR_TYPE_SCALAR && pa->ty.u.scalar == PIR_SCALAR_I32 && !is_signed) return false;
    if (!operand_outside_loop(fn, loop, pa->operands[0])) return false;
    if (!match_unit_stride_offset(fn, body_bid, iv, is_signed, elem_bytes, pa->operands[1],
                                  out_delta, out_narrow_shift, out_off1, out_off2, out_off3)) {
        return false;
    }
    *out_base = pa->operands[0];
    *out_pa   = vd->def_inst;
    return true;
}

static bool is_vectorizable_lane_op(const PirInst *ins, uint32_t elem_bytes) {
    uint32_t trap_flags = PIR_FLAG_TRAP | PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U;
    if ((ins->flags & trap_flags) != 0u) return false;
    if (ins->ty.kind != PIR_TYPE_SCALAR) return false;

    uint32_t bits = elem_bytes * 8u;
    PirOpcode op = ins->op;
    if (elem_bytes <= 2u) {
        /* Conservative SSE2 + NEON intersection for i8/i16 vectors */
        if (op == PIR_OP_ADD || op == PIR_OP_SUB ||
            op == PIR_OP_AND || op == PIR_OP_OR  || op == PIR_OP_XOR) {
            return pir_type_is_int(ins->ty) && pir_scalar_bits(ins->ty.u.scalar, 8u) == bits;
        }
        return false;
    }
    if (op == PIR_OP_FADD || op == PIR_OP_FSUB || op == PIR_OP_FMUL ||
        op == PIR_OP_FDIV || op == PIR_OP_FNEG ||
        (op >= PIR_OP_FCMP_OEQ && op <= PIR_OP_FCMP_UGE)) {
        return pir_type_is_float(ins->ty) && pir_scalar_bits(ins->ty.u.scalar, 8u) == bits;
    }
    if (op == PIR_OP_ADD || op == PIR_OP_SUB || op == PIR_OP_MUL ||
        op == PIR_OP_AND || op == PIR_OP_OR  || op == PIR_OP_XOR ||
        op == PIR_OP_SHL || op == PIR_OP_SHR || op == PIR_OP_SAR ||
        (op >= PIR_OP_CMP_EQ && op <= PIR_OP_CMP_GE_U)) {
        return pir_type_is_int(ins->ty) && pir_scalar_bits(ins->ty.u.scalar, 8u) == bits;
    }
    if (op == PIR_OP_SELECT) {
        return (pir_type_is_float(ins->ty) || pir_type_is_int(ins->ty)) &&
               pir_scalar_bits(ins->ty.u.scalar, 8u) == bits;
    }
    return false;
}

static bool op_same_base(PirOperand a, PirOperand b) {
    if (a.kind != b.kind) return false;
    if (a.kind == PIR_OPER_VAL) return a.u.val == b.u.val;
    if (a.kind == PIR_OPER_SYM) {
        return a.u.sym.offset == b.u.sym.offset && a.u.sym.name && b.u.sym.name &&
               strcmp(a.u.sym.name, b.u.sym.name) == 0;
    }
    return false;
}

typedef enum {
    LV_ALIAS_NONE = 0,       /* Provably disjoint base objects */
    LV_ALIAS_EXACT_SAME = 1, /* Identical element address every iteration (safe RMW) */
    LV_ALIAS_DIFF_OFF = 2,   /* Same base object, close different offset (unsafe recurrence) */
    LV_ALIAS_MAY = 3         /* May alias across iterations -> requires runtime overlap guard */
} LoopVecAlias;

/* Classify whole-array aliasing between two loop base pointers across ALL iterations.
 * Unlike single-element pir_alias_query(base_a, elem_bytes, base_b, elem_bytes) — which
 * wrongly returns NO for `buf + 4` vs `buf` when checking only i=0 — this decomposes to
 * the underlying base object so inlined overlapping slices (e.g. dst = buf + 1, src = buf)
 * are never mis-vectorized! */
static LoopVecAlias classify_loop_pair(const PirFunction *fn,
                                       PirOperand a_base, int64_t a_delta,
                                       PirOperand b_base, int64_t b_delta,
                                       uint32_t elem_bytes) {
    if (op_same_base(a_base, b_base)) {
        return (a_delta == b_delta) ? LV_ALIAS_EXACT_SAME : LV_ALIAS_DIFF_OFF;
    }
    PirPtrInfo pa = pir_alias_decompose(fn, a_base);
    PirPtrInfo pb = pir_alias_decompose(fn, b_base);
    bool same_obj = (pa.kind != PIR_PTR_UNKNOWN && pa.kind == pb.kind) &&
                    ((pa.kind == PIR_PTR_GLOBAL)
                     ? (pa.global_name && pb.global_name && strcmp(pa.global_name, pb.global_name) == 0)
                     : (pa.obj_id == pb.obj_id));
    if (same_obj) {
        if (pa.has_offset && pb.has_offset) {
            int64_t oa = pa.offset + a_delta * (int64_t)elem_bytes;
            int64_t ob = pb.offset + b_delta * (int64_t)elem_bytes;
            if (oa == ob) return LV_ALIAS_EXACT_SAME;
            int64_t dist = (oa > ob) ? (oa - ob) : (ob - oa);
            /* Distance < 2 elements is guaranteed to overlap in any vectorized loop;
             * larger offsets (e.g. disjoint array fields in one struct) use runtime guard. */
            if (dist < (int64_t)elem_bytes * 2) return LV_ALIAS_DIFF_OFF;
            return LV_ALIAS_MAY;
        }
        return LV_ALIAS_MAY;
    }
    if ((pa.kind == PIR_PTR_ALLOCA && pb.kind == PIR_PTR_ALLOCA) ||
        (pa.kind == PIR_PTR_ALLOCA && pb.kind == PIR_PTR_GLOBAL) ||
        (pa.kind == PIR_PTR_GLOBAL && pb.kind == PIR_PTR_ALLOCA) ||
        (pa.kind == PIR_PTR_GLOBAL && pb.kind == PIR_PTR_GLOBAL) ||
        (pa.kind == PIR_PTR_PARAM_NOALIAS && pb.kind != PIR_PTR_UNKNOWN) ||
        (pb.kind == PIR_PTR_PARAM_NOALIAS && pa.kind != PIR_PTR_UNKNOWN)) {
        return LV_ALIAS_NONE;
    }
    return LV_ALIAS_MAY;
}

static bool check_no_external_uses(const PirFunction *fn, PirBlockId body_bid, const bool *in_body) {
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        if (b == body_bid) continue;
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            for (uint32_t k = 0u; k < ins->operand_count; ++k) {
                if (ins->operands[k].kind == PIR_OPER_VAL && ins->operands[k].u.val < fn->values.len &&
                    in_body[ins->operands[k].u.val]) {
                    return false;
                }
            }
            for (uint32_t t = 0u; t < 2u; ++t) {
                for (uint32_t k = 0u; k < ins->targets[t].arg_count; ++k) {
                    if (ins->targets[t].args[k].kind == PIR_OPER_VAL &&
                        ins->targets[t].args[k].u.val < fn->values.len &&
                        in_body[ins->targets[t].args[k].u.val]) {
                        return false;
                    }
                }
            }
            for (uint32_t c = 0u; c < ins->switch_case_count; ++c) {
                for (uint32_t k = 0u; k < ins->switch_cases[c].target.arg_count; ++k) {
                    if (ins->switch_cases[c].target.args[k].kind == PIR_OPER_VAL &&
                        ins->switch_cases[c].target.args[k].u.val < fn->values.len &&
                        in_body[ins->switch_cases[c].target.args[k].u.val]) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static PirValueId get_or_emit_splat(PirModule *mod, PirFunction *fn, PirBlockId pre_bid,
                                    SplatEntry *splats, uint32_t *nsplats, uint32_t max_splats,
                                    PirOperand scalar_op, PirType vec_ty, uint32_t sline) {
    for (uint32_t i = 0u; i < *nsplats; ++i) {
        if (pir_type_eq(splats[i].vec_ty, vec_ty) &&
            splats[i].scalar_op.kind == scalar_op.kind) {
            bool eq = false;
            if (scalar_op.kind == PIR_OPER_VAL) eq = (splats[i].scalar_op.u.val == scalar_op.u.val);
            else if (scalar_op.kind == PIR_OPER_IMM_INT) {
                eq = (splats[i].scalar_op.u.imm_int.lo == scalar_op.u.imm_int.lo &&
                      splats[i].scalar_op.u.imm_int.is_negative == scalar_op.u.imm_int.is_negative);
            } else if (scalar_op.kind == PIR_OPER_IMM_FLOAT) {
                eq = (splats[i].scalar_op.u.imm_float.bits == scalar_op.u.imm_float.bits);
            }
            if (eq) return splats[i].splat_val;
        }
    }
    PirValueId sv = pir_fn_new_val(mod, fn, "vsplat", vec_ty, pre_bid, PIR_INVALID_ID, 0u);
    PirInst sp = pir_make_inst(PIR_OP_SPLAT, vec_ty, sline, 0u);
    sp.operands = pir_alloc_ops(mod, 1u);
    sp.operands[0] = scalar_op;
    sp.operand_count = 1u;
    sp.results[0] = sv;
    pir_fn_append_inst(fn, &fn->blocks.data[pre_bid].insts, sp);
    if (*nsplats < max_splats) {
        splats[*nsplats].scalar_op = scalar_op;
        splats[*nsplats].vec_ty    = vec_ty;
        splats[*nsplats].splat_val = sv;
        (*nsplats)++;
    }
    return sv;
}

static bool try_vectorize_loop(PirModule *mod, PirFunction *fn, const PirTargetSpec *target,
                               const PirCfg *cfg, const PirLoop *loop) {
    if (loop->blocks.len != 2u) return false;
    PirBlockId hdr_bid  = loop->header;
    PirBlockId body_bid = (loop->blocks.data[0] == hdr_bid) ? loop->blocks.data[1] : loop->blocks.data[0];

    const PirBlock *hdr  = &fn->blocks.data[hdr_bid];
    const PirBlock *body = &fn->blocks.data[body_bid];

    /* Header must have 1 IV param (i32 or i64), 2 preds (1 outside + body), and 2 insts (cmp + br). */
    if (hdr->params.len != 1u || hdr->insts.len != 2u) return false;
    if (cfg->preds[hdr_bid].len != 2u) return false;
    if (body->params.len != 0u || body->insts.len < 3u || cfg->preds[body_bid].len != 1u ||
        cfg->preds[body_bid].data[0] != hdr_bid) return false;

    PirBlockId entry_pred = (cfg->preds[hdr_bid].data[0] == body_bid)
                            ? cfg->preds[hdr_bid].data[1]
                            : cfg->preds[hdr_bid].data[0];
    if (entry_pred == body_bid || loop->in_loop[entry_pred]) return false;

    /* Entry predecessor must jump cleanly to header (single target or br whose target matches). */
    const PirBlock *ep_blk = &fn->blocks.data[entry_pred];
    if (ep_blk->insts.len == 0u) return false;
    PirInstId ep_term_id = ep_blk->insts.data[ep_blk->insts.len - 1u];
    const PirInst *ep_term = &fn->insts.data[ep_term_id];
    if (ep_term->op != PIR_OP_JMP || ep_term->targets[0].target != hdr_bid ||
        ep_term->targets[0].arg_count != 1u) return false;
    PirOperand iv_init = ep_term->targets[0].args[0];

    PirValueId iv = hdr->params.data[0];
    PirType iv_ty = fn->values.data[iv].type;
    if (iv_ty.kind != PIR_TYPE_SCALAR ||
        (iv_ty.u.scalar != PIR_SCALAR_I64 && iv_ty.u.scalar != PIR_SCALAR_I32)) return false;

    /* Match header: cmp_lt/le_s/u %iv, %limit; br %c, .body, .exit (or mirrored cmp_ge/gt_s/u). */
    const PirInst *hcmp = &fn->insts.data[hdr->insts.data[0]];
    const PirInst *hbr  = &fn->insts.data[hdr->insts.data[1]];
    if (hbr->op != PIR_OP_BR || hbr->operands[0].kind != PIR_OPER_VAL ||
        hbr->operands[0].u.val != hcmp->results[0]) return false;

    bool is_signed    = false;
    bool is_inclusive = false;
    PirOperand limit_op;
    PirBlockId exit_bid = PIR_INVALID_ID;
    if ((hcmp->op == PIR_OP_CMP_LT_S || hcmp->op == PIR_OP_CMP_LT_U) &&
        hcmp->operands[0].kind == PIR_OPER_VAL && hcmp->operands[0].u.val == iv &&
        hbr->targets[0].target == body_bid && hbr->targets[0].arg_count == 0u) {
        is_signed    = (hcmp->op == PIR_OP_CMP_LT_S);
        is_inclusive = false;
        limit_op     = hcmp->operands[1];
        exit_bid     = hbr->targets[1].target;
    } else if ((hcmp->op == PIR_OP_CMP_GE_S || hcmp->op == PIR_OP_CMP_GE_U) &&
               hcmp->operands[0].kind == PIR_OPER_VAL && hcmp->operands[0].u.val == iv &&
               hbr->targets[1].target == body_bid && hbr->targets[1].arg_count == 0u) {
        is_signed    = (hcmp->op == PIR_OP_CMP_GE_S);
        is_inclusive = false;
        limit_op     = hcmp->operands[1];
        exit_bid     = hbr->targets[0].target;
    } else if ((hcmp->op == PIR_OP_CMP_LE_S || hcmp->op == PIR_OP_CMP_LE_U) &&
               hcmp->operands[0].kind == PIR_OPER_VAL && hcmp->operands[0].u.val == iv &&
               hbr->targets[0].target == body_bid && hbr->targets[0].arg_count == 0u) {
        is_signed    = (hcmp->op == PIR_OP_CMP_LE_S);
        is_inclusive = true;
        limit_op     = hcmp->operands[1];
        exit_bid     = hbr->targets[1].target;
    } else if ((hcmp->op == PIR_OP_CMP_GT_S || hcmp->op == PIR_OP_CMP_GT_U) &&
               hcmp->operands[0].kind == PIR_OPER_VAL && hcmp->operands[0].u.val == iv &&
               hbr->targets[1].target == body_bid && hbr->targets[1].arg_count == 0u) {
        is_signed    = (hcmp->op == PIR_OP_CMP_GT_S);
        is_inclusive = true;
        limit_op     = hcmp->operands[1];
        exit_bid     = hbr->targets[0].target;
    } else {
        return false;
    }
    if (exit_bid == PIR_INVALID_ID || loop->in_loop[exit_bid]) return false;
    if (!operand_outside_loop(fn, loop, limit_op)) return false;

    /* Body terminator must be `jmp .header(%iv_next)` where `%iv_next = add %iv, 1`. */
    const PirInst *bterm = &fn->insts.data[body->insts.data[body->insts.len - 1u]];
    if (bterm->op != PIR_OP_JMP || bterm->targets[0].target != hdr_bid ||
        bterm->targets[0].arg_count != 1u || bterm->targets[0].args[0].kind != PIR_OPER_VAL) {
        return false;
    }
    PirValueId iv_next = bterm->targets[0].args[0].u.val;
    if (iv_next >= fn->values.len || fn->values.data[iv_next].def_block != body_bid ||
        fn->values.data[iv_next].def_inst == PIR_INVALID_ID) return false;
    PirInstId iv_step_id = fn->values.data[iv_next].def_inst;
    const PirInst *step_ins = &fn->insts.data[iv_step_id];
    if (step_ins->op != PIR_OP_ADD || (step_ins->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u) {
        return false;
    }
    uint64_t step_c = 0u;
    bool step_ok = (step_ins->operands[0].kind == PIR_OPER_VAL && step_ins->operands[0].u.val == iv &&
                    op_is_imm_u64(step_ins->operands[1], &step_c) && step_c == 1u) ||
                   (step_ins->operands[1].kind == PIR_OPER_VAL && step_ins->operands[1].u.val == iv &&
                    op_is_imm_u64(step_ins->operands[0], &step_c) && step_c == 1u);
    if (!step_ok) return false;

    /* Analyze all body instructions. */
    uint32_t nb = body->insts.len - 1u; /* excluding terminator */
    if (nb > 64u) return false; /* keep codegen tight & predictable */

    uint32_t elem_bytes = 0u, vf = 0u;
    PirVecType dummy_vt = PIR_VEC_V4F32;
    uint32_t store_cnt = 0u, load_cnt = 0u;
    MemAccessDesc store_descs[16];
    MemAccessDesc load_descs[16];
    bool    has_narrow_delta = false;
    int64_t min_narrow_delta = 0;
    int64_t max_narrow_delta = 0;

    BodyInstInfo *binfo = (BodyInstInfo *)calloc(nb, sizeof(BodyInstInfo));
    bool *in_body_val   = (bool *)calloc(fn->values.len + 1u, sizeof(bool));
    bool *is_addr_inst  = (bool *)calloc(fn->insts.len + 1u, sizeof(bool));
    bool *is_vec_val    = (bool *)calloc(fn->values.len + 1u, sizeof(bool));
    if (!binfo || !in_body_val || !is_addr_inst || !is_vec_val) abort();

    bool legal = true;

    /* Pass 1: Identify loads/stores, uniform element byte size, and unit-stride address chains. */
    for (uint32_t j = 0u; j < nb; ++j) {
        PirInstId iid = body->insts.data[j];
        const PirInst *ins = &fn->insts.data[iid];
        if (ins->results[0] != PIR_INVALID_ID && ins->results[0] < fn->values.len) {
            if (iid != iv_step_id) in_body_val[ins->results[0]] = true;
        }
        if (ins->results[1] != PIR_INVALID_ID) { legal = false; break; }

        if (ins->op == PIR_OP_LOAD || ins->op == PIR_OP_STORE) {
            if ((ins->flags & PIR_FLAG_VOLATILE) != 0u) { legal = false; break; }
            uint32_t eb = 0u, lvf = 0u;
            PirVecType lvt;
            if (!is_vectorizable_elem_type(ins->ty, &eb, &lvf, &lvt)) { legal = false; break; }
            if (elem_bytes == 0u) {
                elem_bytes = eb; vf = lvf; dummy_vt = lvt;
            } else if (elem_bytes != eb) {
                legal = false; break;
            }
            PirOperand pop = (ins->op == PIR_OP_LOAD) ? ins->operands[0] : ins->operands[1];
            PirOperand base;
            int64_t delta = 0;
            bool narrow_shift = false;
            PirInstId pa_id, off1, off2, off3;
            if (!match_unit_stride_ptr(fn, loop, body_bid, iv, is_signed, elem_bytes, pop,
                                       &base, &delta, &narrow_shift, &pa_id, &off1, &off2, &off3)) {
                legal = false; break;
            }
            if (narrow_shift && delta != 0) {
                has_narrow_delta = true;
                if (delta < min_narrow_delta) min_narrow_delta = delta;
                if (delta > max_narrow_delta) max_narrow_delta = delta;
            }
            is_addr_inst[pa_id] = true;
            if (off1 != PIR_INVALID_ID) is_addr_inst[off1] = true;
            if (off2 != PIR_INVALID_ID) is_addr_inst[off2] = true;
            if (off3 != PIR_INVALID_ID) is_addr_inst[off3] = true;
            binfo[j].is_vec_op = true;
            binfo[j].mem_base  = base;
            binfo[j].mem_delta = delta;
            if (ins->op == PIR_OP_STORE) {
                if (store_cnt >= 16u) { legal = false; break; }
                store_descs[store_cnt].base  = base;
                store_descs[store_cnt].delta = delta;
                store_cnt++;
            } else {
                if (load_cnt >= 16u) { legal = false; break; }
                load_descs[load_cnt].base  = base;
                load_descs[load_cnt].delta = delta;
                load_cnt++;
            }
        }
    }

    if (!legal || store_cnt == 0u || elem_bytes == 0u) goto cleanup;

    /* Pass 2: Alias safety check (static PIR_ALIAS_NO or up to 4 runtime non-overlap guards). */
    RuntimeAliasPair rt_pairs[4];
    uint32_t rt_pair_cnt = 0u;

    for (uint32_t si = 0u; si < store_cnt && legal; ++si) {
        for (uint32_t sj = si + 1u; sj < store_cnt; ++sj) {
            LoopVecAlias al = classify_loop_pair(fn, store_descs[si].base, store_descs[si].delta,
                                                 store_descs[sj].base, store_descs[sj].delta,
                                                 elem_bytes);
            if (al == LV_ALIAS_NONE || al == LV_ALIAS_EXACT_SAME) continue;
            if (al == LV_ALIAS_DIFF_OFF) { legal = false; break; }
            if (rt_pair_cnt >= 4u) { legal = false; break; }
            rt_pairs[rt_pair_cnt].a_base  = store_descs[si].base;
            rt_pairs[rt_pair_cnt].a_delta = store_descs[si].delta;
            rt_pairs[rt_pair_cnt].b_base  = store_descs[sj].base;
            rt_pairs[rt_pair_cnt].b_delta = store_descs[sj].delta;
            rt_pair_cnt++;
        }
        for (uint32_t li = 0u; li < load_cnt && legal; ++li) {
            LoopVecAlias al = classify_loop_pair(fn, store_descs[si].base, store_descs[si].delta,
                                                 load_descs[li].base, load_descs[li].delta,
                                                 elem_bytes);
            /* Exact same-index RMW (a[i+d] = f(a[i+d])) or disjoint bases have zero dependence! */
            if (al == LV_ALIAS_NONE || al == LV_ALIAS_EXACT_SAME) continue;
            if (al == LV_ALIAS_DIFF_OFF) { legal = false; break; }
            /* Deduplicate identical runtime check pairs */
            bool dup = false;
            for (uint32_t r = 0u; r < rt_pair_cnt; ++r) {
                if (op_same_base(rt_pairs[r].a_base, store_descs[si].base) &&
                    rt_pairs[r].a_delta == store_descs[si].delta &&
                    op_same_base(rt_pairs[r].b_base, load_descs[li].base) &&
                    rt_pairs[r].b_delta == load_descs[li].delta) {
                    dup = true; break;
                }
            }
            if (!dup) {
                if (rt_pair_cnt >= 4u) { legal = false; break; }
                rt_pairs[rt_pair_cnt].a_base  = store_descs[si].base;
                rt_pairs[rt_pair_cnt].a_delta = store_descs[si].delta;
                rt_pairs[rt_pair_cnt].b_base  = load_descs[li].base;
                rt_pairs[rt_pair_cnt].b_delta = load_descs[li].delta;
                rt_pair_cnt++;
            }
        }
    }
    if (!legal) goto cleanup;

    /* Pass 3: Verify every body instruction is either addr-calc, iv-step, or vectorizable lane op. */
    for (uint32_t j = 0u; j < nb && legal; ++j) {
        PirInstId iid = body->insts.data[j];
        const PirInst *ins = &fn->insts.data[iid];
        if (iid == iv_step_id) {
            binfo[j].is_iv_step = true;
            continue;
        }
        if (is_addr_inst[iid]) {
            binfo[j].is_mem_addr = true;
            continue;
        }
        if (ins->op == PIR_OP_LOAD) {
            is_vec_val[ins->results[0]] = true;
            continue;
        }
        if (ins->op == PIR_OP_STORE) {
            PirOperand sv = ins->operands[0];
            if (sv.kind == PIR_OPER_VAL && !operand_outside_loop(fn, loop, sv) && !is_vec_val[sv.u.val]) {
                legal = false;
            }
            continue;
        }
        if (!is_vectorizable_lane_op(ins, elem_bytes)) {
            legal = false; break;
        }
        /* All operands must be either vectorized body values or loop-invariant scalars! */
        for (uint32_t k = 0u; k < ins->operand_count; ++k) {
            PirOperand op = ins->operands[k];
            if (ins->op == PIR_OP_SELECT && k == 0u) {
                /* Condition must be a vectorized comparison mask from body */
                if (op.kind != PIR_OPER_VAL || !is_vec_val[op.u.val]) { legal = false; break; }
            } else if (op.kind == PIR_OPER_VAL) {
                if (!operand_outside_loop(fn, loop, op) && !is_vec_val[op.u.val]) {
                    legal = false; break;
                }
            }
        }
        if (legal) {
            binfo[j].is_vec_op = true;
            is_vec_val[ins->results[0]] = true;
        }
    }
    if (!legal) goto cleanup;

    /* Pass 4: No value defined in body (except iv_next on backedge) may escape the loop. */
    if (!check_no_external_uses(fn, body_bid, in_body_val)) goto cleanup;

    /* =====================================================================
     * Transformation: Emit guard -> vec_pre -> vec_hdr -> vec_body (2x/4x unrolled)
     *                 and fall through to original scalar header(.header(%viv))!
     * ===================================================================== */
    {
        (void)dummy_vt;
        /* Target-register-budget aware interleave/unroll factor to avoid SIMD register spills. */
        uint32_t avail_vregs = (target->arch == PIR_ARCH_ARM64) ? 24u : 12u;
        uint32_t est_vregs   = (load_cnt > 0u ? load_cnt : 1u) + 1u;
        bool fits_4x_regs    = (4u * est_vregs + 2u <= avail_vregs);
        const uint32_t unroll = (fits_4x_regs &&
                                 ((vf >= 4u && nb <= 20u) ||
                                  (vf == 2u && nb <= 14u && load_cnt <= 3u && store_cnt <= 2u)))
                                ? 4u : 2u;
        const uint32_t step   = vf * unroll; /* 64 for i8, 32 for i16, 16 for f32/i32, 8 or 4 for f64/i64 */
        /* Skip emitting dead vector guards when constant trip count is provably < step. */
        {
            uint64_t c_init = 0u, c_lim = 0u;
            bool has_ci = op_is_imm_u64(iv_init, &c_init);
            if (!has_ci && iv_init.kind == PIR_OPER_VAL && iv_init.u.val < fn->values.len) {
                PirInstId di = fn->values.data[iv_init.u.val].def_inst;
                if (di != PIR_INVALID_ID && fn->insts.data[di].op == PIR_OP_CONST) {
                    has_ci = op_is_imm_u64(fn->insts.data[di].operands[0], &c_init);
                }
            }
            bool has_cl = op_is_imm_u64(limit_op, &c_lim);
            if (!has_cl && limit_op.kind == PIR_OPER_VAL && limit_op.u.val < fn->values.len) {
                PirInstId di = fn->values.data[limit_op.u.val].def_inst;
                if (di != PIR_INVALID_ID && fn->insts.data[di].op == PIR_OP_CONST) {
                    has_cl = op_is_imm_u64(fn->insts.data[di].operands[0], &c_lim);
                }
            }
            if (has_ci && has_cl) {
                int64_t si = (iv_ty.u.scalar == PIR_SCALAR_I32) ? (int64_t)(int32_t)c_init : (int64_t)c_init;
                int64_t sl = (iv_ty.u.scalar == PIR_SCALAR_I32) ? (int64_t)(int32_t)c_lim  : (int64_t)c_lim;
                uint64_t trips = 0u;
                if (is_signed) {
                    if (sl > si || (is_inclusive && sl == si)) {
                        trips = (uint64_t)(sl - si) + (is_inclusive ? 1u : 0u);
                    }
                } else {
                    if (c_lim > c_init || (is_inclusive && c_lim == c_init)) {
                        trips = (c_lim - c_init) + (is_inclusive ? 1u : 0u);
                    }
                }
                if (trips < (uint64_t)step) goto cleanup;
            }
        }
        const uint32_t shift_amt = (elem_bytes == 1u) ? 0u
                                 : (elem_bytes == 2u) ? 1u
                                 : (elem_bytes == 4u) ? 2u : 3u;
        const uint32_t vec_bytes = vf * elem_bytes; /* 16 bytes */
        uint32_t sline = hdr->src_line;

        PirBlockId guard_bid   = fn->blocks.len;
        PirBlockId vpre_bid    = guard_bid + 1u;
        PirBlockId vhdr_bid    = guard_bid + 2u;
        PirBlockId vbody_bid   = guard_bid + 3u;

        static const char *const sfx[4] = { "vguard", "vpre", "vhdr", "vbody" };
        for (uint32_t k = 0u; k < 4u; ++k) {
            char lbuf[64];
            snprintf(lbuf, sizeof(lbuf), "%s_%u", sfx[k], guard_bid + k);
            PirBlock nb_blk;
            memset(&nb_blk, 0, sizeof(nb_blk));
            nb_blk.label    = pir_arena_strdup(&mod->arena, lbuf);
            nb_blk.src_line = sline;
            PIR_VEC_PUSH(&fn->blocks, nb_blk);
        }

        /* Redirect entry_pred's jump from hdr_bid -> guard_bid (0 args). */
        fn->insts.data[ep_term_id].targets[0].target    = guard_bid;
        fn->insts.data[ep_term_id].targets[0].args      = NULL;
        fn->insts.data[ep_term_id].targets[0].arg_count = 0u;

        /* Guard block: normalize inclusive limit to exclusive `excl_limit = limit + 1` (guarded
         * against MAX_VAL overflow), then check (iv_init < excl_limit) && ((unsigned)(excl_limit - iv_init) >= step). */
        PirType i8_ty  = pir_type_scalar(PIR_SCALAR_I8);
        PirType i64_ty = pir_type_scalar(PIR_SCALAR_I64);
        PirType ptr_ty = pir_type_scalar(PIR_SCALAR_PTR);

        PirOperand excl_limit = limit_op;
        PirValueId c_ok = PIR_INVALID_ID;

        if (is_inclusive) {
            uint64_t max_val = 0u;
            if (iv_ty.u.scalar == PIR_SCALAR_I32) {
                max_val = is_signed ? 0x7FFFFFFFu : 0xFFFFFFFFu;
            } else {
                max_val = is_signed ? 0x7FFFFFFFFFFFFFFFULL : 0xFFFFFFFFFFFFFFFFULL;
            }
            PirValueId c_max = pir_fn_new_val(mod, fn, "vg_max", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
            PirInst i_max = pir_make_inst(is_signed ? PIR_OP_CMP_LT_S : PIR_OP_CMP_LT_U, iv_ty, sline, 0u);
            i_max.operands = pir_alloc_ops(mod, 2u);
            i_max.operands[0] = limit_op;
            i_max.operands[1] = pir_op_imm_i(max_val, false, iv_ty);
            i_max.operand_count = 2u;
            i_max.results[0] = c_max;
            pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_max);

            PirValueId elim_v = pir_fn_new_val(mod, fn, "vg_elim", iv_ty, guard_bid, PIR_INVALID_ID, 0u);
            PirInst i_elim = pir_make_inst(PIR_OP_ADD, iv_ty, sline, 0u);
            i_elim.flags = PIR_FLAG_WRAP;
            i_elim.operands = pir_alloc_ops(mod, 2u);
            i_elim.operands[0] = limit_op;
            i_elim.operands[1] = pir_op_imm_i(1u, false, iv_ty);
            i_elim.operand_count = 2u;
            i_elim.results[0] = elim_v;
            pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_elim);

            excl_limit = pir_op_val(elim_v, iv_ty);
            c_ok = c_max;
        }

        PirValueId c_lt = pir_fn_new_val(mod, fn, "vg_lt", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
        PirInst i_lt = pir_make_inst(is_signed ? PIR_OP_CMP_LT_S : PIR_OP_CMP_LT_U, iv_ty, sline, 0u);
        i_lt.operands = pir_alloc_ops(mod, 2u);
        i_lt.operands[0] = iv_init;
        i_lt.operands[1] = excl_limit;
        i_lt.operand_count = 2u;
        i_lt.results[0] = c_lt;
        pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_lt);

        PirValueId v_diff = pir_fn_new_val(mod, fn, "vg_diff", iv_ty, guard_bid, PIR_INVALID_ID, 0u);
        PirInst i_diff = pir_make_inst(PIR_OP_SUB, iv_ty, sline, 0u);
        i_diff.flags = PIR_FLAG_WRAP;
        i_diff.operands = pir_alloc_ops(mod, 2u);
        i_diff.operands[0] = excl_limit;
        i_diff.operands[1] = iv_init;
        i_diff.operand_count = 2u;
        i_diff.results[0] = v_diff;
        pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_diff);

        PirValueId c_ge = pir_fn_new_val(mod, fn, "vg_ge", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
        PirInst i_ge = pir_make_inst(PIR_OP_CMP_GE_U, iv_ty, sline, 0u);
        i_ge.operands = pir_alloc_ops(mod, 2u);
        i_ge.operands[0] = pir_op_val(v_diff, iv_ty);
        i_ge.operands[1] = pir_op_imm_i((uint64_t)step, false, iv_ty);
        i_ge.operand_count = 2u;
        i_ge.results[0] = c_ge;
        pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_ge);

        PirValueId c_ok0 = pir_fn_new_val(mod, fn, "vg_ok", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
        PirInst i_ok = pir_make_inst(PIR_OP_AND, i8_ty, sline, 0u);
        i_ok.operands = pir_alloc_ops(mod, 2u);
        i_ok.operands[0] = pir_op_val(c_lt, i8_ty);
        i_ok.operands[1] = pir_op_val(c_ge, i8_ty);
        i_ok.operand_count = 2u;
        i_ok.results[0] = c_ok0;
        pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_ok);

        if (c_ok != PIR_INVALID_ID) {
            PirValueId c_and = pir_fn_new_val(mod, fn, "vg_oka", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
            PirInst i_and = pir_make_inst(PIR_OP_AND, i8_ty, sline, 0u);
            i_and.operands = pir_alloc_ops(mod, 2u);
            i_and.operands[0] = pir_op_val(c_ok, i8_ty);
            i_and.operands[1] = pir_op_val(c_ok0, i8_ty);
            i_and.operand_count = 2u;
            i_and.results[0] = c_and;
            pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_and);
            c_ok = c_and;
        } else {
            c_ok = c_ok0;
        }

        /* Guard against i32 wraparound when `sext(iv + delta)` was hoisted across an i32 shift. */
        if (has_narrow_delta && iv_ty.u.scalar == PIR_SCALAR_I32) {
            if (min_narrow_delta < 0) {
                uint64_t lo_bound = is_signed
                    ? (uint64_t)(-(int64_t)0x80000000LL - min_narrow_delta) /* INT32_MIN + |d| */
                    : (uint64_t)(-min_narrow_delta);
                bool lo_neg = is_signed && ((int64_t)(-2147483648LL - min_narrow_delta) < 0);
                uint64_t lo_mag = lo_neg ? (uint64_t)(2147483648LL + min_narrow_delta) : lo_bound;
                PirValueId c_dlo = pir_fn_new_val(mod, fn, "vg_dlo", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst i_dlo = pir_make_inst(is_signed ? PIR_OP_CMP_GE_S : PIR_OP_CMP_GE_U, iv_ty, sline, 0u);
                i_dlo.operands = pir_alloc_ops(mod, 2u);
                i_dlo.operands[0] = iv_init;
                i_dlo.operands[1] = pir_op_imm_i(lo_mag, lo_neg, iv_ty);
                i_dlo.operand_count = 2u;
                i_dlo.results[0] = c_dlo;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_dlo);

                PirValueId c_n = pir_fn_new_val(mod, fn, "vg_okd", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst i_n = pir_make_inst(PIR_OP_AND, i8_ty, sline, 0u);
                i_n.operands = pir_alloc_ops(mod, 2u);
                i_n.operands[0] = pir_op_val(c_ok, i8_ty);
                i_n.operands[1] = pir_op_val(c_dlo, i8_ty);
                i_n.operand_count = 2u;
                i_n.results[0] = c_n;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_n);
                c_ok = c_n;
            }
            if (max_narrow_delta > 0) {
                uint64_t hi_bound = (is_signed ? 0x7FFFFFFFu : 0xFFFFFFFFu) - (uint64_t)max_narrow_delta + 1u;
                PirValueId c_dhi = pir_fn_new_val(mod, fn, "vg_dhi", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst i_dhi = pir_make_inst(is_signed ? PIR_OP_CMP_LE_S : PIR_OP_CMP_LE_U, iv_ty, sline, 0u);
                i_dhi.operands = pir_alloc_ops(mod, 2u);
                i_dhi.operands[0] = excl_limit;
                i_dhi.operands[1] = pir_op_imm_i(hi_bound, false, iv_ty);
                i_dhi.operand_count = 2u;
                i_dhi.results[0] = c_dhi;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_dhi);

                PirValueId c_n = pir_fn_new_val(mod, fn, "vg_okh", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst i_n = pir_make_inst(PIR_OP_AND, i8_ty, sline, 0u);
                i_n.operands = pir_alloc_ops(mod, 2u);
                i_n.operands[0] = pir_op_val(c_ok, i8_ty);
                i_n.operands[1] = pir_op_val(c_dhi, i8_ty);
                i_n.operand_count = 2u;
                i_n.results[0] = c_n;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, i_n);
                c_ok = c_n;
            }
        }

        /* Emit runtime non-overlap alias checks in guard_bid if static alias analysis returned MAY. */
        if (rt_pair_cnt > 0u) {
            PirOperand iv0_64 = iv_init;
            PirOperand lim_64 = excl_limit;
            if (iv_ty.u.scalar == PIR_SCALAR_I32) {
                PirOpcode ext_op = is_signed ? PIR_OP_SEXT : PIR_OP_ZEXT;
                if (iv_init.kind == PIR_OPER_IMM_INT) {
                    if (is_signed) {
                        int64_t sv = iv_init.u.imm_int.is_negative
                                     ? -(int64_t)(uint32_t)iv_init.u.imm_int.lo
                                     : (int64_t)(int32_t)(uint32_t)iv_init.u.imm_int.lo;
                        iv0_64 = pir_op_imm_i(sv < 0 ? (uint64_t)(-sv) : (uint64_t)sv, sv < 0, i64_ty);
                    } else {
                        uint64_t uv = iv_init.u.imm_int.is_negative
                                      ? ((uint64_t)(0u - (uint32_t)iv_init.u.imm_int.lo))
                                      : (iv_init.u.imm_int.lo & 0xFFFFFFFFULL);
                        iv0_64 = pir_op_imm_i(uv, false, i64_ty);
                    }
                } else {
                    PirValueId v0 = pir_fn_new_val(mod, fn, "vg_i0", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                    PirInst e0 = pir_make_inst(ext_op, i64_ty, sline, 0u);
                    e0.operands = pir_alloc_ops(mod, 1u);
                    e0.operands[0] = iv_init;
                    e0.operand_count = 1u;
                    e0.results[0] = v0;
                    pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, e0);
                    iv0_64 = pir_op_val(v0, i64_ty);
                }

                if (excl_limit.kind == PIR_OPER_IMM_INT) {
                    if (is_signed) {
                        int64_t sv = excl_limit.u.imm_int.is_negative
                                     ? -(int64_t)(uint32_t)excl_limit.u.imm_int.lo
                                     : (int64_t)(int32_t)(uint32_t)excl_limit.u.imm_int.lo;
                        lim_64 = pir_op_imm_i(sv < 0 ? (uint64_t)(-sv) : (uint64_t)sv, sv < 0, i64_ty);
                    } else {
                        uint64_t uv = excl_limit.u.imm_int.is_negative
                                      ? ((uint64_t)(0u - (uint32_t)excl_limit.u.imm_int.lo))
                                      : (excl_limit.u.imm_int.lo & 0xFFFFFFFFULL);
                        lim_64 = pir_op_imm_i(uv, false, i64_ty);
                    }
                } else {
                    PirValueId v1 = pir_fn_new_val(mod, fn, "vg_lm", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                    PirInst e1 = pir_make_inst(ext_op, i64_ty, sline, 0u);
                    e1.operands = pir_alloc_ops(mod, 1u);
                    e1.operands[0] = excl_limit;
                    e1.operand_count = 1u;
                    e1.results[0] = v1;
                    pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, e1);
                    lim_64 = pir_op_val(v1, i64_ty);
                }
            }

            PirOperand iv0_by = iv0_64;
            PirOperand lim_by = lim_64;
            if (shift_amt > 0u) {
                PirValueId s0 = pir_fn_new_val(mod, fn, "vg_b0", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst sh0 = pir_make_inst(PIR_OP_SHL, i64_ty, sline, 0u);
                sh0.flags = PIR_FLAG_MASK;
                sh0.operands = pir_alloc_ops(mod, 2u);
                sh0.operands[0] = iv0_64;
                sh0.operands[1] = pir_op_imm_i((uint64_t)shift_amt, false, i64_ty);
                sh0.operand_count = 2u;
                sh0.results[0] = s0;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, sh0);
                iv0_by = pir_op_val(s0, i64_ty);

                PirValueId s1 = pir_fn_new_val(mod, fn, "vg_b1", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst sh1 = pir_make_inst(PIR_OP_SHL, i64_ty, sline, 0u);
                sh1.flags = PIR_FLAG_MASK;
                sh1.operands = pir_alloc_ops(mod, 2u);
                sh1.operands[0] = lim_64;
                sh1.operands[1] = pir_op_imm_i((uint64_t)shift_amt, false, i64_ty);
                sh1.operand_count = 2u;
                sh1.results[0] = s1;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, sh1);
                lim_by = pir_op_val(s1, i64_ty);
            }

            for (uint32_t rp = 0u; rp < rt_pair_cnt; ++rp) {
                PirValueId a_int = pir_fn_new_val(mod, fn, "vg_ai", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst pa_i = pir_make_inst(PIR_OP_PTRTOINT, i64_ty, sline, 0u);
                pa_i.operands = pir_alloc_ops(mod, 1u);
                pa_i.operands[0] = rt_pairs[rp].a_base;
                pa_i.operand_count = 1u;
                pa_i.results[0] = a_int;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, pa_i);

                PirValueId b_int = pir_fn_new_val(mod, fn, "vg_bi", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst pb_i = pir_make_inst(PIR_OP_PTRTOINT, i64_ty, sline, 0u);
                pb_i.operands = pir_alloc_ops(mod, 1u);
                pb_i.operands[0] = rt_pairs[rp].b_base;
                pb_i.operand_count = 1u;
                pb_i.results[0] = b_int;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, pb_i);

                PirValueId a_adj = a_int;
                if (rt_pairs[rp].a_delta != 0) {
                    int64_t db = rt_pairs[rp].a_delta * (int64_t)elem_bytes;
                    a_adj = pir_fn_new_val(mod, fn, "vg_aa", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                    PirInst da = pir_make_inst(PIR_OP_ADD, i64_ty, sline, 0u);
                    da.flags = PIR_FLAG_WRAP;
                    da.operands = pir_alloc_ops(mod, 2u);
                    da.operands[0] = pir_op_val(a_int, i64_ty);
                    da.operands[1] = pir_op_imm_i(db < 0 ? (uint64_t)(-db) : (uint64_t)db, db < 0, i64_ty);
                    da.operand_count = 2u;
                    da.results[0] = a_adj;
                    pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, da);
                }
                PirValueId b_adj = b_int;
                if (rt_pairs[rp].b_delta != 0) {
                    int64_t db = rt_pairs[rp].b_delta * (int64_t)elem_bytes;
                    b_adj = pir_fn_new_val(mod, fn, "vg_ba", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                    PirInst db_i = pir_make_inst(PIR_OP_ADD, i64_ty, sline, 0u);
                    db_i.flags = PIR_FLAG_WRAP;
                    db_i.operands = pir_alloc_ops(mod, 2u);
                    db_i.operands[0] = pir_op_val(b_int, i64_ty);
                    db_i.operands[1] = pir_op_imm_i(db < 0 ? (uint64_t)(-db) : (uint64_t)db, db < 0, i64_ty);
                    db_i.operand_count = 2u;
                    db_i.results[0] = b_adj;
                    pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, db_i);
                }

                PirValueId a_lo = pir_fn_new_val(mod, fn, "vg_alo", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst ial = pir_make_inst(PIR_OP_ADD, i64_ty, sline, 0u);
                ial.flags = PIR_FLAG_WRAP; ial.operands = pir_alloc_ops(mod, 2u);
                ial.operands[0] = pir_op_val(a_adj, i64_ty); ial.operands[1] = iv0_by;
                ial.operand_count = 2u; ial.results[0] = a_lo;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, ial);

                PirValueId a_hi = pir_fn_new_val(mod, fn, "vg_ahi", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst iah = pir_make_inst(PIR_OP_ADD, i64_ty, sline, 0u);
                iah.flags = PIR_FLAG_WRAP; iah.operands = pir_alloc_ops(mod, 2u);
                iah.operands[0] = pir_op_val(a_adj, i64_ty); iah.operands[1] = lim_by;
                iah.operand_count = 2u; iah.results[0] = a_hi;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, iah);

                PirValueId b_lo = pir_fn_new_val(mod, fn, "vg_blo", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst ibl = pir_make_inst(PIR_OP_ADD, i64_ty, sline, 0u);
                ibl.flags = PIR_FLAG_WRAP; ibl.operands = pir_alloc_ops(mod, 2u);
                ibl.operands[0] = pir_op_val(b_adj, i64_ty); ibl.operands[1] = iv0_by;
                ibl.operand_count = 2u; ibl.results[0] = b_lo;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, ibl);

                PirValueId b_hi = pir_fn_new_val(mod, fn, "vg_bhi", i64_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst ibh = pir_make_inst(PIR_OP_ADD, i64_ty, sline, 0u);
                ibh.flags = PIR_FLAG_WRAP; ibh.operands = pir_alloc_ops(mod, 2u);
                ibh.operands[0] = pir_op_val(b_adj, i64_ty); ibh.operands[1] = lim_by;
                ibh.operand_count = 2u; ibh.results[0] = b_hi;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, ibh);

                /* Disjoint iff (a_hi <=_u b_lo) || (b_hi <=_u a_lo) and neither range wrapped! */
                PirValueId d1 = pir_fn_new_val(mod, fn, "vg_d1", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst id1 = pir_make_inst(PIR_OP_CMP_LE_U, i64_ty, sline, 0u);
                id1.operands = pir_alloc_ops(mod, 2u);
                id1.operands[0] = pir_op_val(a_hi, i64_ty); id1.operands[1] = pir_op_val(b_lo, i64_ty);
                id1.operand_count = 2u; id1.results[0] = d1;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, id1);

                PirValueId d2 = pir_fn_new_val(mod, fn, "vg_d2", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst id2 = pir_make_inst(PIR_OP_CMP_LE_U, i64_ty, sline, 0u);
                id2.operands = pir_alloc_ops(mod, 2u);
                id2.operands[0] = pir_op_val(b_hi, i64_ty); id2.operands[1] = pir_op_val(a_lo, i64_ty);
                id2.operand_count = 2u; id2.results[0] = d2;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, id2);

                PirValueId disj = pir_fn_new_val(mod, fn, "vg_dj", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst idj = pir_make_inst(PIR_OP_OR, i8_ty, sline, 0u);
                idj.operands = pir_alloc_ops(mod, 2u);
                idj.operands[0] = pir_op_val(d1, i8_ty); idj.operands[1] = pir_op_val(d2, i8_ty);
                idj.operand_count = 2u; idj.results[0] = disj;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, idj);

                PirValueId nw1 = pir_fn_new_val(mod, fn, "vg_nw1", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst inw1 = pir_make_inst(PIR_OP_CMP_LE_U, i64_ty, sline, 0u);
                inw1.operands = pir_alloc_ops(mod, 2u);
                inw1.operands[0] = pir_op_val(a_lo, i64_ty); inw1.operands[1] = pir_op_val(a_hi, i64_ty);
                inw1.operand_count = 2u; inw1.results[0] = nw1;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, inw1);

                PirValueId nw2 = pir_fn_new_val(mod, fn, "vg_nw2", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst inw2 = pir_make_inst(PIR_OP_CMP_LE_U, i64_ty, sline, 0u);
                inw2.operands = pir_alloc_ops(mod, 2u);
                inw2.operands[0] = pir_op_val(b_lo, i64_ty); inw2.operands[1] = pir_op_val(b_hi, i64_ty);
                inw2.operand_count = 2u; inw2.results[0] = nw2;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, inw2);

                PirValueId nw = pir_fn_new_val(mod, fn, "vg_nw", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst inw = pir_make_inst(PIR_OP_AND, i8_ty, sline, 0u);
                inw.operands = pir_alloc_ops(mod, 2u);
                inw.operands[0] = pir_op_val(nw1, i8_ty); inw.operands[1] = pir_op_val(nw2, i8_ty);
                inw.operand_count = 2u; inw.results[0] = nw;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, inw);

                PirValueId pair_ok = pir_fn_new_val(mod, fn, "vg_pok", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst ipok = pir_make_inst(PIR_OP_AND, i8_ty, sline, 0u);
                ipok.operands = pir_alloc_ops(mod, 2u);
                ipok.operands[0] = pir_op_val(disj, i8_ty); ipok.operands[1] = pir_op_val(nw, i8_ty);
                ipok.operand_count = 2u; ipok.results[0] = pair_ok;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, ipok);

                PirValueId c_next = pir_fn_new_val(mod, fn, "vg_okp", i8_ty, guard_bid, PIR_INVALID_ID, 0u);
                PirInst inx = pir_make_inst(PIR_OP_AND, i8_ty, sline, 0u);
                inx.operands = pir_alloc_ops(mod, 2u);
                inx.operands[0] = pir_op_val(c_ok, i8_ty); inx.operands[1] = pir_op_val(pair_ok, i8_ty);
                inx.operand_count = 2u; inx.results[0] = c_next;
                pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, inx);
                c_ok = c_next;
            }
        }

        PirInst gbr = pir_make_inst(PIR_OP_BR, pir_type_scalar(PIR_SCALAR_VOID), sline, 0u);
        gbr.operands = pir_alloc_ops(mod, 1u);
        gbr.operands[0] = pir_op_val(c_ok, i8_ty);
        gbr.operand_count = 1u;
        gbr.targets[0].target    = vpre_bid;
        gbr.targets[0].arg_count = 0u;
        gbr.targets[1].target    = hdr_bid;
        gbr.targets[1].args      = pir_alloc_ops(mod, 1u);
        gbr.targets[1].args[0]   = iv_init;
        gbr.targets[1].arg_count = 1u;
        pir_fn_append_inst(fn, &fn->blocks.data[guard_bid].insts, gbr);

        /* Preheader: compute vlimit = excl_limit - (step - 1) and splat loop-invariant operands. */
        PirValueId vlimit = pir_fn_new_val(mod, fn, "vlim", iv_ty, vpre_bid, PIR_INVALID_ID, 0u);
        PirInst i_vlim = pir_make_inst(PIR_OP_SUB, iv_ty, sline, 0u);
        i_vlim.flags = PIR_FLAG_WRAP;
        i_vlim.operands = pir_alloc_ops(mod, 2u);
        i_vlim.operands[0] = excl_limit;
        i_vlim.operands[1] = pir_op_imm_i((uint64_t)(step - 1u), false, iv_ty);
        i_vlim.operand_count = 2u;
        i_vlim.results[0] = vlimit;
        pir_fn_append_inst(fn, &fn->blocks.data[vpre_bid].insts, i_vlim);

        /* Pre-emit splats into vpre_bid, and save vec value mappings per unroll lane. */
        SplatEntry splats[32];
        uint32_t nsplats = 0u;

        /* Vector header: .vhdr(%viv: iv_ty): %vc = cmp_ge_s/u %viv, %vlim; br %vc, .hdr(%viv), .vbody
         * (putting .vbody as the false target makes it the zero-branch fallthrough!). */
        PirValueId viv = pir_fn_new_val(mod, fn, "viv", iv_ty, vhdr_bid, PIR_INVALID_ID, 0u);
        PIR_VEC_PUSH(&fn->blocks.data[vhdr_bid].params, viv);

        PirValueId vcond = pir_fn_new_val(mod, fn, "vcmp", i8_ty, vhdr_bid, PIR_INVALID_ID, 0u);
        PirInst i_vcmp = pir_make_inst(is_signed ? PIR_OP_CMP_GE_S : PIR_OP_CMP_GE_U, iv_ty, sline, 0u);
        i_vcmp.operands = pir_alloc_ops(mod, 2u);
        i_vcmp.operands[0] = pir_op_val(viv, iv_ty);
        i_vcmp.operands[1] = pir_op_val(vlimit, iv_ty);
        i_vcmp.operand_count = 2u;
        i_vcmp.results[0] = vcond;
        pir_fn_append_inst(fn, &fn->blocks.data[vhdr_bid].insts, i_vcmp);

        PirInst vbr = pir_make_inst(PIR_OP_BR, pir_type_scalar(PIR_SCALAR_VOID), sline, 0u);
        vbr.operands = pir_alloc_ops(mod, 1u);
        vbr.operands[0] = pir_op_val(vcond, i8_ty);
        vbr.operand_count = 1u;
        vbr.targets[0].target    = hdr_bid;
        vbr.targets[0].args      = pir_alloc_ops(mod, 1u);
        vbr.targets[0].args[0]   = pir_op_val(viv, iv_ty);
        vbr.targets[0].arg_count = 1u;
        vbr.targets[1].target    = vbody_bid;
        vbr.targets[1].arg_count = 0u;
        pir_fn_append_inst(fn, &fn->blocks.data[vhdr_bid].insts, vbr);

        /* Vector body: compute byte offset = (i64)viv << shift_amt. */
        PirValueId viv64 = viv;
        if (iv_ty.u.scalar == PIR_SCALAR_I32) {
            viv64 = pir_fn_new_val(mod, fn, "viv64", i64_ty, vbody_bid, PIR_INVALID_ID, 0u);
            PirInst ext = pir_make_inst(is_signed ? PIR_OP_SEXT : PIR_OP_ZEXT, i64_ty, sline, 0u);
            ext.operands = pir_alloc_ops(mod, 1u);
            ext.operands[0] = pir_op_val(viv, iv_ty);
            ext.operand_count = 1u;
            ext.results[0] = viv64;
            pir_fn_append_inst(fn, &fn->blocks.data[vbody_bid].insts, ext);
        }

        PirValueId voff = viv64;
        if (shift_amt > 0u) {
            voff = pir_fn_new_val(mod, fn, "voff", i64_ty, vbody_bid, PIR_INVALID_ID, 0u);
            PirInst i_off = pir_make_inst(PIR_OP_SHL, i64_ty, sline, 0u);
            i_off.flags = PIR_FLAG_MASK;
            i_off.operands = pir_alloc_ops(mod, 2u);
            i_off.operands[0] = pir_op_val(viv64, i64_ty);
            i_off.operands[1] = pir_op_imm_i((uint64_t)shift_amt, false, i64_ty);
            i_off.operand_count = 2u;
            i_off.results[0] = voff;
            pir_fn_append_inst(fn, &fn->blocks.data[vbody_bid].insts, i_off);
        }

        /* Cache `(base + delta*elem_bytes) + voff` once per unique (base, delta) pair! */
        MemAccessDesc uniq_bases[32];
        PirValueId base_ptrs[32];
        uint32_t nuniq_bases = 0u;

        /* Snapshot body instruction IDs (vectorizing appends new insts to fn->insts). */
        PirInstId body_iids[64];
        for (uint32_t j = 0u; j < nb; ++j) body_iids[j] = fn->blocks.data[body_bid].insts.data[j];

        uint32_t orig_nvals = fn->values.len;
        PirValueId *vmap[4] = { NULL, NULL, NULL, NULL };
        for (uint32_t u = 0u; u < unroll; ++u) {
            vmap[u] = (PirValueId *)malloc(orig_nvals * sizeof(PirValueId));
            if (!vmap[u]) abort();
            for (uint32_t v = 0u; v < orig_nvals; ++v) vmap[u][v] = PIR_INVALID_ID;
        }

        /* Interleave unrolled streams across each operation (j outer, u inner) so independent
         * SIMD lanes execute in parallel on out-of-order vector pipes without false reg reuse. */
        for (uint32_t j = 0u; j < nb; ++j) {
            if (!binfo[j].is_vec_op) continue;
            PirInst orig = fn->insts.data[body_iids[j]];

            if (orig.op == PIR_OP_LOAD || orig.op == PIR_OP_STORE) {
                /* Compute adj_base + voff once per unique (base, delta) pair */
                uint32_t bi = nuniq_bases;
                for (uint32_t k = 0u; k < nuniq_bases; ++k) {
                    if (op_same_base(uniq_bases[k].base, binfo[j].mem_base) &&
                        uniq_bases[k].delta == binfo[j].mem_delta) {
                        bi = k; break;
                    }
                }
                if (bi == nuniq_bases) {
                    PirOperand eff_base = binfo[j].mem_base;
                    if (binfo[j].mem_delta != 0) {
                        /* Pre-adjust base once in vpre_bid for non-zero-based indexing (e.g. Ada 1-based)! */
                        int64_t byte_delta = binfo[j].mem_delta * (int64_t)elem_bytes;
                        PirValueId ab = pir_fn_new_val(mod, fn, "vab", ptr_ty, vpre_bid, PIR_INVALID_ID, 0u);
                        PirInst pab = pir_make_inst(PIR_OP_PTR_ADD, i64_ty, sline, 0u);
                        pab.operands = pir_alloc_ops(mod, 2u);
                        pab.operands[0] = binfo[j].mem_base;
                        pab.operands[1] = pir_op_imm_i(
                            byte_delta < 0 ? (uint64_t)(-byte_delta) : (uint64_t)byte_delta,
                            byte_delta < 0, i64_ty);
                        pab.operand_count = 2u;
                        pab.results[0] = ab;
                        pir_fn_append_inst(fn, &fn->blocks.data[vpre_bid].insts, pab);
                        eff_base = pir_op_val(ab, ptr_ty);
                    }
                    PirValueId bp = pir_fn_new_val(mod, fn, "vbp", ptr_ty, vbody_bid, PIR_INVALID_ID, 0u);
                    PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, i64_ty, orig.src_line, 0u);
                    /* When eff_base was pre-shifted by negative mem_delta, eff_base may sit
                     * before object offset 0 until voff brings it back in-bounds, so omit [inbounds]. */
                    pa.flags = (binfo[j].mem_delta == 0) ? PIR_FLAG_INBOUNDS : PIR_FLAG_NONE;
                    pa.operands = pir_alloc_ops(mod, 2u);
                    pa.operands[0] = eff_base;
                    pa.operands[1] = pir_op_val(voff, i64_ty);
                    pa.operand_count = 2u;
                    pa.results[0] = bp;
                    pir_fn_append_inst(fn, &fn->blocks.data[vbody_bid].insts, pa);
                    uniq_bases[bi].base  = binfo[j].mem_base;
                    uniq_bases[bi].delta = binfo[j].mem_delta;
                    base_ptrs[bi]        = bp;
                    nuniq_bases++;
                }

                PirType vty = pir_type_vec(vec_type_for_scalar_lane(orig.ty.u.scalar, vf));
                for (uint32_t u = 0u; u < unroll; ++u) {
                    PirValueId mem_ptr = base_ptrs[bi];
                    if (u > 0u) {
                        PirValueId bp2 = pir_fn_new_val(mod, fn, "vbp", ptr_ty, vbody_bid, PIR_INVALID_ID, 0u);
                        PirInst pa2 = pir_make_inst(PIR_OP_PTR_ADD, i64_ty, orig.src_line, 0u);
                        pa2.flags = PIR_FLAG_INBOUNDS;
                        pa2.operands = pir_alloc_ops(mod, 2u);
                        pa2.operands[0] = pir_op_val(mem_ptr, ptr_ty);
                        pa2.operands[1] = pir_op_imm_i((uint64_t)(u * vec_bytes), false, i64_ty);
                        pa2.operand_count = 2u;
                        pa2.results[0] = bp2;
                        pir_fn_append_inst(fn, &fn->blocks.data[vbody_bid].insts, pa2);
                        mem_ptr = bp2;
                    }
                    if (orig.op == PIR_OP_LOAD) {
                        PirValueId rv = pir_fn_new_val(mod, fn, "vld", vty, vbody_bid, PIR_INVALID_ID, 0u);
                        PirInst ld = pir_make_inst(PIR_OP_LOAD, vty, orig.src_line, 0u);
                        ld.align = orig.align;
                        ld.operands = pir_alloc_ops(mod, 1u);
                        ld.operands[0] = pir_op_val(mem_ptr, ptr_ty);
                        ld.operand_count = 1u;
                        ld.results[0] = rv;
                        pir_fn_append_inst(fn, &fn->blocks.data[vbody_bid].insts, ld);
                        vmap[u][orig.results[0]] = rv;
                    } else {
                        PirOperand sv = orig.operands[0];
                        PirValueId vsv = PIR_INVALID_ID;
                        if (sv.kind == PIR_OPER_VAL && sv.u.val < orig_nvals && vmap[u][sv.u.val] != PIR_INVALID_ID) {
                            vsv = vmap[u][sv.u.val];
                        } else {
                            vsv = get_or_emit_splat(mod, fn, vpre_bid, splats, &nsplats, 32u, sv, vty, sline);
                        }
                        PirInst st = pir_make_inst(PIR_OP_STORE, vty, orig.src_line, 0u);
                        st.align = orig.align;
                        st.operands = pir_alloc_ops(mod, 2u);
                        st.operands[0] = pir_op_val(vsv, vty);
                        st.operands[1] = pir_op_val(mem_ptr, ptr_ty);
                        st.operand_count = 2u;
                        pir_fn_append_inst(fn, &fn->blocks.data[vbody_bid].insts, st);
                    }
                }
                continue;
            }

            /* Lane-wise vector op interleaved across all unrolled streams */
            PirVecType vvt = vec_type_for_scalar_lane(orig.ty.u.scalar, vf);
            PirType vop_ty = pir_type_vec(vvt);
            PirType vres_ty = vop_ty;
            if ((orig.op >= PIR_OP_CMP_EQ && orig.op <= PIR_OP_CMP_GE_U) ||
                (orig.op >= PIR_OP_FCMP_OEQ && orig.op <= PIR_OP_FCMP_UGE)) {
                vres_ty = pir_type_vec(pir_vec_mask_type(vvt));
            }
            for (uint32_t u = 0u; u < unroll; ++u) {
                PirInst vi = pir_make_inst(orig.op, vop_ty, orig.src_line, 0u);
                vi.flags = orig.flags;
                vi.operands = pir_alloc_ops(mod, orig.operand_count);
                vi.operand_count = orig.operand_count;
                for (uint32_t k = 0u; k < orig.operand_count; ++k) {
                    PirOperand op = orig.operands[k];
                    if (op.kind == PIR_OPER_VAL && op.u.val < orig_nvals && vmap[u][op.u.val] != PIR_INVALID_ID) {
                        PirValueId mv = vmap[u][op.u.val];
                        vi.operands[k] = pir_op_val(mv, fn->values.data[mv].type);
                    } else {
                        PirValueId spv = get_or_emit_splat(mod, fn, vpre_bid, splats, &nsplats, 32u, op, vop_ty, sline);
                        vi.operands[k] = pir_op_val(spv, vop_ty);
                    }
                }
                PirValueId rv = pir_fn_new_val(mod, fn, "vop", vres_ty, vbody_bid, PIR_INVALID_ID, 0u);
                vi.results[0] = rv;
                pir_fn_append_inst(fn, &fn->blocks.data[vbody_bid].insts, vi);
                vmap[u][orig.results[0]] = rv;
            }
        }

        for (uint32_t u = 0u; u < unroll; ++u) free(vmap[u]);

        /* Terminate vpre_bid -> jmp vhdr_bid(iv_init) (after all splats have been emitted!). */
        PirInst pre_jmp = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID), sline, 0u);
        pre_jmp.targets[0].target    = vhdr_bid;
        pre_jmp.targets[0].args      = pir_alloc_ops(mod, 1u);
        pre_jmp.targets[0].args[0]   = iv_init;
        pre_jmp.targets[0].arg_count = 1u;
        pir_fn_append_inst(fn, &fn->blocks.data[vpre_bid].insts, pre_jmp);

        /* Advance viv += step and jump back to vhdr_bid. */
        PirValueId viv_next = pir_fn_new_val(mod, fn, "viv1", iv_ty, vbody_bid, PIR_INVALID_ID, 0u);
        PirInst i_step = pir_make_inst(PIR_OP_ADD, iv_ty, sline, 0u);
        i_step.flags = PIR_FLAG_WRAP;
        i_step.operands = pir_alloc_ops(mod, 2u);
        i_step.operands[0] = pir_op_val(viv, iv_ty);
        i_step.operands[1] = pir_op_imm_i((uint64_t)step, false, iv_ty);
        i_step.operand_count = 2u;
        i_step.results[0] = viv_next;
        pir_fn_append_inst(fn, &fn->blocks.data[vbody_bid].insts, i_step);

        PirInst back_jmp = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID), sline, 0u);
        back_jmp.targets[0].target    = vhdr_bid;
        back_jmp.targets[0].args      = pir_alloc_ops(mod, 1u);
        back_jmp.targets[0].args[0]   = pir_op_val(viv_next, iv_ty);
        back_jmp.targets[0].arg_count = 1u;
        pir_fn_append_inst(fn, &fn->blocks.data[vbody_bid].insts, back_jmp);
    }

    free(binfo);
    free(in_body_val);
    free(is_addr_inst);
    free(is_vec_val);
    return true;

cleanup:
    free(binfo);
    free(in_body_val);
    free(is_addr_inst);
    free(is_vec_val);
    return false;
}

void pir_opt_loop_vec(PirModule *mod, PirFunction *fn, const PirTargetSpec *target) {
    if (fn->blocks.len < 2u) return;
    if (target->caps.max_simd_bits < 128u) return;

    /* Vectorize at most 16 loops per function (rebuild CFG/loops cleanly after each). */
    for (uint32_t round = 0u; round < 16u; ++round) {
        PirCfg cfg;
        PirDom dom;
        PirLoopInfo li;
        pir_cfg_build(&cfg, fn);
        pir_dom_build(&dom, &cfg);
        pir_loops_build(&li, &cfg, &dom);

        bool changed = false;
        for (uint32_t i = 0u; i < li.loops.len; ++i) {
            if (try_vectorize_loop(mod, fn, target, &cfg, &li.loops.data[i])) {
                changed = true;
                break;
            }
        }

        pir_loops_destroy(&li);
        pir_dom_destroy(&dom);
        pir_cfg_destroy(&cfg);
        if (!changed) break;
    }
}
