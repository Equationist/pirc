/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "opt/opt.h"

#include <stdlib.h>
#include <string.h>

#include "analysis/cfg.h"
#include "analysis/dom.h"
#include "lower/lower.h"

typedef struct {
    bool       is_int;
    uint64_t   bits;
} ConstVal;

typedef struct {
    PirOpcode  op;
    PirType    ty;
    uint32_t   flags;
    uint64_t   imm_u64;
    uint32_t   op_count;
    PirOperand ops[3];
    PirValueId val_id;
    uint32_t   hash;
    uint32_t   prev;
} GvnEntry;

typedef PIR_VEC(GvnEntry) GvnEntryVec;

typedef struct {
    PirModule           *mod;
    PirFunction         *fn;
    const PirTargetSpec *target;
    const PirDom        *dom;
    PirValueId          *alias;
    ConstVal            *consts;
    uint32_t             nvals;
    GvnEntryVec          table;
    uint32_t            *buckets;
    uint32_t             bucket_cap;
} GvnCtx;

static uint32_t gvn_mix(uint32_t h, uint64_t v) {
    h ^= (uint32_t)v;
    h *= 0x85ebca6bu;
    h ^= (uint32_t)(v >> 32u);
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16u);
}

static uint32_t gvn_hash_inst(const PirInst *ins, const PirOperand *canon) {
    uint32_t h = 2166136261u;
    h = gvn_mix(h, (uint64_t)ins->op);
    h = gvn_mix(h, (uint64_t)ins->ty.kind);
    h = gvn_mix(h, (uint64_t)ins->ty.u.agg_id);
    h = gvn_mix(h, (uint64_t)ins->flags);
    h = gvn_mix(h, ins->imm_u64);
    h = gvn_mix(h, (uint64_t)ins->operand_count);
    for (uint32_t k = 0u; k < ins->operand_count; ++k) {
        const PirOperand *op = &canon[k];
        h = gvn_mix(h, (uint64_t)op->kind);
        switch (op->kind) {
            case PIR_OPER_VAL:
                h = gvn_mix(h, (uint64_t)op->u.val);
                break;
            case PIR_OPER_IMM_INT:
                h = gvn_mix(h, op->u.imm_int.lo);
                h = gvn_mix(h, op->u.imm_int.hi ^ (uint64_t)op->u.imm_int.is_negative);
                h = gvn_mix(h, (uint64_t)op->type.u.scalar);
                break;
            case PIR_OPER_IMM_FLOAT:
                h = gvn_mix(h, op->u.imm_float.bits);
                h = gvn_mix(h, (uint64_t)op->type.u.scalar);
                break;
            case PIR_OPER_SYM:
                h = gvn_mix(h, (uint64_t)op->u.sym.offset);
                if (op->u.sym.name) {
                    for (const unsigned char *s = (const unsigned char *)op->u.sym.name; *s; ++s) {
                        h = (h ^ (uint32_t)*s) * 16777619u;
                    }
                }
                break;
            case PIR_OPER_INVOKE_RET:
            case PIR_OPER_INVOKE_EXN:
                break;
        }
    }
    return h;
}

static void gvn_rehash(GvnCtx *ctx, uint32_t new_cap) {
    uint32_t *nb = (uint32_t *)malloc(new_cap * sizeof(uint32_t));
    if (!nb) abort();
    for (uint32_t i = 0u; i < new_cap; ++i) nb[i] = PIR_INVALID_ID;
    uint32_t mask = new_cap - 1u;
    for (uint32_t i = 0u; i < ctx->table.len; ++i) {
        uint32_t bi = ctx->table.data[i].hash & mask;
        ctx->table.data[i].prev = nb[bi];
        nb[bi] = i;
    }
    free(ctx->buckets);
    ctx->buckets    = nb;
    ctx->bucket_cap = new_cap;
}

static PirValueId resolve_alias(GvnCtx *ctx, PirValueId v) {
    PirValueId root = v;
    uint32_t steps = 0u;
    while (root < ctx->nvals && ctx->alias[root] != PIR_INVALID_ID && steps < ctx->nvals) {
        root = ctx->alias[root];
        steps++;
    }
    PirValueId cur = v;
    while (cur != root && cur < ctx->nvals && ctx->alias[cur] != PIR_INVALID_ID) {
        PirValueId next = ctx->alias[cur];
        ctx->alias[cur] = root;
        cur = next;
    }
    return root;
}

static void resolve_ops(GvnCtx *ctx, PirOperand *ops, uint32_t n) {
    for (uint32_t i = 0u; i < n; ++i) {
        if (ops[i].kind == PIR_OPER_VAL) {
            ops[i].u.val = resolve_alias(ctx, ops[i].u.val);
        }
    }
}

static uint32_t scalar_int_bits(PirType ty) {
    if (ty.kind != PIR_TYPE_SCALAR || !pir_scalar_is_int(ty.u.scalar)) return 0u;
    uint32_t b = pir_scalar_bits(ty.u.scalar, 8u);
    return (b <= 64u) ? b : 0u;
}

static uint64_t mask_bits(uint32_t bits) {
    return (bits >= 64u) ? UINT64_MAX : ((1ULL << bits) - 1ULL);
}

static int64_t sext_bits(uint64_t v, uint32_t bits) {
    if (bits >= 64u) return (int64_t)v;
    uint64_t sign = 1ULL << (bits - 1u);
    v &= mask_bits(bits);
    if (v & sign) v |= ~mask_bits(bits);
    return (int64_t)v;
}

static bool get_int_const(const GvnCtx *ctx, PirOperand op, uint32_t bits, uint64_t *out) {
    uint64_t raw = 0u;
    if (op.kind == PIR_OPER_IMM_INT) {
        raw = op.u.imm_int.is_negative ? (0u - op.u.imm_int.lo) : op.u.imm_int.lo;
    } else if (op.kind == PIR_OPER_VAL && op.u.val < ctx->nvals && ctx->consts[op.u.val].is_int) {
        raw = ctx->consts[op.u.val].bits;
    } else {
        return false;
    }
    *out = raw & mask_bits(bits);
    return true;
}

static bool is_commutative(const PirInst *ins) {
    PirOpcode op = ins->op;
    if (op == PIR_OP_FADD || op == PIR_OP_FMUL) {
        /* IEEE-754 NaN payload propagation is order-dependent unless [fast] is set. */
        return (ins->flags & PIR_FLAG_FAST) != 0u;
    }
    return op == PIR_OP_ADD || op == PIR_OP_MUL || op == PIR_OP_AND || op == PIR_OP_OR ||
           op == PIR_OP_XOR ||
           op == PIR_OP_CMP_EQ || op == PIR_OP_CMP_NE ||
           op == PIR_OP_FCMP_OEQ || op == PIR_OP_FCMP_ONE ||
           op == PIR_OP_FCMP_UEQ || op == PIR_OP_FCMP_UNE;
}

static bool op_less(PirOperand a, PirOperand b) {
    if (a.kind != b.kind) return (int)a.kind < (int)b.kind;
    switch (a.kind) {
        case PIR_OPER_VAL:       return a.u.val < b.u.val;
        case PIR_OPER_IMM_INT:
            if (a.u.imm_int.is_negative != b.u.imm_int.is_negative) {
                return a.u.imm_int.is_negative < b.u.imm_int.is_negative;
            }
            if (a.u.imm_int.hi != b.u.imm_int.hi) return a.u.imm_int.hi < b.u.imm_int.hi;
            if (a.u.imm_int.lo != b.u.imm_int.lo) return a.u.imm_int.lo < b.u.imm_int.lo;
            return (int)a.type.u.scalar < (int)b.type.u.scalar;
        case PIR_OPER_IMM_FLOAT:
            if (a.u.imm_float.bits != b.u.imm_float.bits) {
                return a.u.imm_float.bits < b.u.imm_float.bits;
            }
            return (int)a.type.u.scalar < (int)b.type.u.scalar;
        case PIR_OPER_SYM:
            if (a.u.sym.offset != b.u.sym.offset) return a.u.sym.offset < b.u.sym.offset;
            return (a.u.sym.name && b.u.sym.name) ? (strcmp(a.u.sym.name, b.u.sym.name) < 0) : false;
        case PIR_OPER_INVOKE_RET:
        case PIR_OPER_INVOKE_EXN: return false;
    }
    return false;
}

static bool is_gvn_eligible(const PirModule *mod, const PirFunction *fn, const PirInst *ins) {
    if (ins->results[0] == PIR_INVALID_ID || ins->results[1] != PIR_INVALID_ID) return false;
    if (ins->operand_count > 3u) return false;
    if ((ins->flags & (PIR_FLAG_INBOUNDS | PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) != 0u) {
        return false;
    }
    if (ins->op == PIR_OP_INTTOPTR || ins->op == PIR_OP_SHUFFLE) return false;
    if (ins->op == PIR_OP_BITCAST &&
        ins->ty.kind == PIR_TYPE_SCALAR && ins->ty.u.scalar == PIR_SCALAR_PTR &&
        (ins->operand_count < 1u ||
         !(ins->operands[0].type.kind == PIR_TYPE_SCALAR &&
           ins->operands[0].type.u.scalar == PIR_SCALAR_PTR))) {
        return false;
    }
    uint32_t eff = pir_inst_effects(mod, fn, ins);
    if ((eff & PIR_EFF_PURE_HOISTABLE) != 0u) return true;
    /* Dominated CSE of `select` does not speculate, so it needs only clean flags. */
    return ins->op == PIR_OP_SELECT &&
           (eff & (PIR_EFF_MAY_TRAP | PIR_EFF_HAS_UB_FLAG)) == 0u &&
           (ins->flags & PIR_FLAG_VOLATILE) == 0u;
}

static void rewrite_to_const_int(GvnCtx *ctx, PirInst *ins, uint64_t val, PirType ty) {
    ins->op            = PIR_OP_CONST;
    ins->ty            = ty;
    ins->flags         = PIR_FLAG_NONE;
    ins->operands      = pir_alloc_ops(ctx->mod, 1u);
    ins->operands[0]   = pir_op_imm_i(val, false, ty);
    ins->operand_count = 1u;
    if (ins->results[0] < ctx->nvals) {
        ctx->consts[ins->results[0]].is_int = true;
        ctx->consts[ins->results[0]].bits   = val;
    }
}

static bool has_ovf_flag(uint32_t f) {
    return (f & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U | PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) != 0u;
}

static bool try_fold_const_int_op(GvnCtx *ctx, PirInst *ins) {
    uint32_t bits = scalar_int_bits(ins->ty);
    if (bits == 0u) return false;
    uint64_t mask = mask_bits(bits);

    /* Unary integer casts on constants (zext/sext/trunc) */
    if (ins->operand_count == 1u &&
        (ins->op == PIR_OP_ZEXT || ins->op == PIR_OP_SEXT || ins->op == PIR_OP_TRUNC)) {
        uint32_t sbits = scalar_int_bits(ins->operands[0].type);
        uint64_t a = 0u;
        if (sbits > 0u && get_int_const(ctx, ins->operands[0], sbits, &a)) {
            uint64_t r = 0u;
            if (ins->op == PIR_OP_SEXT) {
                r = (uint64_t)sext_bits(a, sbits) & mask;
            } else {
                r = a & mask;
            }
            rewrite_to_const_int(ctx, ins, r, ins->ty);
            return true;
        }
        return false;
    }

    /* Binary scalar int ops */
    if (ins->operand_count == 2u) {
        uint64_t a = 0u, b = 0u;
        if (!get_int_const(ctx, ins->operands[0], bits, &a) ||
            !get_int_const(ctx, ins->operands[1], bits, &b)) {
            return false;
        }
        int64_t sa = sext_bits(a, bits);
        int64_t sb = sext_bits(b, bits);
        int64_t smin = (bits == 64u) ? INT64_MIN : -(1LL << (bits - 1u));
        int64_t smax = (bits == 64u) ? INT64_MAX : ((1LL << (bits - 1u)) - 1LL);

        if (ins->op == PIR_OP_ADD) {
            if (ins->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_UNSAFE_S)) {
                if ((sb > 0 && sa > smax - sb) || (sb < 0 && sa < smin - sb)) return false;
            }
            if (ins->flags & (PIR_FLAG_TRAP_U | PIR_FLAG_UNSAFE_U)) {
                if (a > mask - b) return false;
            }
            rewrite_to_const_int(ctx, ins, (a + b) & mask, ins->ty);
            return true;
        }
        if (ins->op == PIR_OP_SUB) {
            if (ins->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_UNSAFE_S)) {
                if ((sb < 0 && sa > smax + sb) || (sb > 0 && sa < smin + sb)) return false;
            }
            if (ins->flags & (PIR_FLAG_TRAP_U | PIR_FLAG_UNSAFE_U)) {
                if (a < b) return false;
            }
            rewrite_to_const_int(ctx, ins, (a - b) & mask, ins->ty);
            return true;
        }
        if (ins->op == PIR_OP_MUL) {
            if (has_ovf_flag(ins->flags)) {
                /* Conservative: only fold when no overflow (simple safe check via 64-bit for <64). */
                if (bits >= 64u && a != 0u && b != 0u) return false;
                if (ins->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_UNSAFE_S)) {
                    int64_t p = sa * sb;
                    if (p < smin || p > smax) return false;
                }
                if (ins->flags & (PIR_FLAG_TRAP_U | PIR_FLAG_UNSAFE_U)) {
                    if (a * b > mask) return false;
                }
            }
            rewrite_to_const_int(ctx, ins, (a * b) & mask, ins->ty);
            return true;
        }
        if (ins->op == PIR_OP_DIV_S || ins->op == PIR_OP_REM_S) {
            if (sb == 0 || (sa == smin && sb == -1)) return false; /* Preserve trap/UB */
            uint64_t r = (ins->op == PIR_OP_DIV_S) ? (uint64_t)(sa / sb) : (uint64_t)(sa % sb);
            rewrite_to_const_int(ctx, ins, r & mask, ins->ty);
            return true;
        }
        if (ins->op == PIR_OP_DIV_U || ins->op == PIR_OP_REM_U) {
            if (b == 0u) return false;
            uint64_t r = (ins->op == PIR_OP_DIV_U) ? (a / b) : (a % b);
            rewrite_to_const_int(ctx, ins, r & mask, ins->ty);
            return true;
        }
        if (ins->op == PIR_OP_AND) { rewrite_to_const_int(ctx, ins, (a & b) & mask, ins->ty); return true; }
        if (ins->op == PIR_OP_OR)  { rewrite_to_const_int(ctx, ins, (a | b) & mask, ins->ty); return true; }
        if (ins->op == PIR_OP_XOR) { rewrite_to_const_int(ctx, ins, (a ^ b) & mask, ins->ty); return true; }
        if (ins->op == PIR_OP_SHL || ins->op == PIR_OP_SHR || ins->op == PIR_OP_SAR) {
            if ((ins->flags & (PIR_FLAG_TRAP | PIR_FLAG_UNSAFE)) && b >= (uint64_t)bits) return false;
            uint32_t sh = (uint32_t)(b & (uint64_t)(bits - 1u));
            uint64_t r = 0u;
            if (ins->op == PIR_OP_SHL) {
                r = (a << sh) & mask;
            } else if (ins->op == PIR_OP_SHR) {
                r = (a & mask) >> sh;
            } else {
                r = (sa < 0) ? ((~((~a & mask) >> sh)) & mask) : ((a & mask) >> sh);
            }
            rewrite_to_const_int(ctx, ins, r, ins->ty);
            return true;
        }
        /* Integer comparisons produce i8 */
        PirType i8 = pir_type_scalar(PIR_SCALAR_I8);
        if (ins->op == PIR_OP_CMP_EQ)   { rewrite_to_const_int(ctx, ins, (a == b) ? 1u : 0u, i8); return true; }
        if (ins->op == PIR_OP_CMP_NE)   { rewrite_to_const_int(ctx, ins, (a != b) ? 1u : 0u, i8); return true; }
        if (ins->op == PIR_OP_CMP_LT_S) { rewrite_to_const_int(ctx, ins, (sa < sb) ? 1u : 0u, i8); return true; }
        if (ins->op == PIR_OP_CMP_LT_U) { rewrite_to_const_int(ctx, ins, (a < b) ? 1u : 0u, i8); return true; }
        if (ins->op == PIR_OP_CMP_LE_S) { rewrite_to_const_int(ctx, ins, (sa <= sb) ? 1u : 0u, i8); return true; }
        if (ins->op == PIR_OP_CMP_LE_U) { rewrite_to_const_int(ctx, ins, (a <= b) ? 1u : 0u, i8); return true; }
        if (ins->op == PIR_OP_CMP_GT_S) { rewrite_to_const_int(ctx, ins, (sa > sb) ? 1u : 0u, i8); return true; }
        if (ins->op == PIR_OP_CMP_GT_U) { rewrite_to_const_int(ctx, ins, (a > b) ? 1u : 0u, i8); return true; }
        if (ins->op == PIR_OP_CMP_GE_S) { rewrite_to_const_int(ctx, ins, (sa >= sb) ? 1u : 0u, i8); return true; }
        if (ins->op == PIR_OP_CMP_GE_U) { rewrite_to_const_int(ctx, ins, (a >= b) ? 1u : 0u, i8); return true; }
    }
    return false;
}

static bool is_bool_producing_op(PirOpcode op) {
    return (op >= PIR_OP_CMP_EQ && op <= PIR_OP_CMP_GE_U) ||
           (op >= PIR_OP_FCMP_OEQ && op <= PIR_OP_FCMP_UGE);
}

static bool invert_int_cmp(PirOpcode op, PirOpcode *out) {
    if (op == PIR_OP_CMP_EQ)   { *out = PIR_OP_CMP_NE;   return true; }
    if (op == PIR_OP_CMP_NE)   { *out = PIR_OP_CMP_EQ;   return true; }
    if (op == PIR_OP_CMP_LT_S) { *out = PIR_OP_CMP_GE_S; return true; }
    if (op == PIR_OP_CMP_LE_S) { *out = PIR_OP_CMP_GT_S; return true; }
    if (op == PIR_OP_CMP_GT_S) { *out = PIR_OP_CMP_LE_S; return true; }
    if (op == PIR_OP_CMP_GE_S) { *out = PIR_OP_CMP_LT_S; return true; }
    if (op == PIR_OP_CMP_LT_U) { *out = PIR_OP_CMP_GE_U; return true; }
    if (op == PIR_OP_CMP_LE_U) { *out = PIR_OP_CMP_GT_U; return true; }
    if (op == PIR_OP_CMP_GT_U) { *out = PIR_OP_CMP_LE_U; return true; }
    if (op == PIR_OP_CMP_GE_U) { *out = PIR_OP_CMP_LT_U; return true; }
    return false;
}

static bool swap_int_cmp(PirOpcode op, PirOpcode *out) {
    if (op == PIR_OP_CMP_EQ)   { *out = PIR_OP_CMP_EQ;   return true; }
    if (op == PIR_OP_CMP_NE)   { *out = PIR_OP_CMP_NE;   return true; }
    if (op == PIR_OP_CMP_LT_S) { *out = PIR_OP_CMP_GT_S; return true; }
    if (op == PIR_OP_CMP_LE_S) { *out = PIR_OP_CMP_GE_S; return true; }
    if (op == PIR_OP_CMP_GT_S) { *out = PIR_OP_CMP_LT_S; return true; }
    if (op == PIR_OP_CMP_GE_S) { *out = PIR_OP_CMP_LE_S; return true; }
    if (op == PIR_OP_CMP_LT_U) { *out = PIR_OP_CMP_GT_U; return true; }
    if (op == PIR_OP_CMP_LE_U) { *out = PIR_OP_CMP_GE_U; return true; }
    if (op == PIR_OP_CMP_GT_U) { *out = PIR_OP_CMP_LT_U; return true; }
    if (op == PIR_OP_CMP_GE_U) { *out = PIR_OP_CMP_LE_U; return true; }
    return false;
}

static PirOperand make_canon_imm(uint64_t v, uint32_t bits, PirType ty) {
    uint64_t mask = mask_bits(bits);
    uint64_t m = v & mask;
    return pir_op_imm_i(m, false, ty);
}

static bool fits_arm64_imm_for_op(PirOpcode op, uint64_t val, uint32_t bits) {
    uint64_t m = val & mask_bits(bits);
    if (op == PIR_OP_ADD || op == PIR_OP_SUB ||
        op == PIR_OP_ADD_OVF_S || op == PIR_OP_ADD_OVF_U ||
        op == PIR_OP_SUB_OVF_S || op == PIR_OP_SUB_OVF_U ||
        (op >= PIR_OP_CMP_EQ && op <= PIR_OP_CMP_GE_U)) {
        return m <= 4095u;
    }
    if (op == PIR_OP_SHL || op == PIR_OP_SHR || op == PIR_OP_SAR) {
        return m < (uint64_t)bits;
    }
    if (op == PIR_OP_AND || op == PIR_OP_OR || op == PIR_OP_XOR) {
        uint32_t wbits = (bits <= 32u) ? 32u : 64u;
        uint64_t full = (wbits == 64u) ? UINT64_MAX : ((1ULL << wbits) - 1ULL);
        uint64_t v = m & full;
        if (v == 0u || v == full) return false;
        uint64_t lowest = v & (0u - v);
        uint64_t sum = v + lowest;
        return (sum & (sum - 1u)) == 0u;
    }
    return false;
}

/* Fold constant %val operands directly into immediate operand slots when they fit
 * native ARM64 immediates (leaving large constants as PIR_OP_CONST so LICM hoists them!). */
static void propagate_int_imms(GvnCtx *ctx, PirInst *ins) {
    if (ins->operand_count != 2u) return;

    if (ins->op == PIR_OP_PTR_ADD) {
        uint32_t pbits = scalar_int_bits(ins->ty);
        if (pbits == 0u) pbits = (uint32_t)ctx->target->ptr_bytes * 8u;
        uint64_t off = 0u;
        if (ins->operands[1].kind == PIR_OPER_VAL &&
            get_int_const(ctx, ins->operands[1], pbits, &off)) {
            int64_t soff = sext_bits(off, pbits);
            if (soff >= -256 && soff <= 32760) {
                if (soff < 0) {
                    ins->operands[1] = pir_op_imm_i((uint64_t)(-soff), true, ins->operands[1].type);
                } else {
                    ins->operands[1] = pir_op_imm_i(off, false, ins->operands[1].type);
                }
            }
        }
        return;
    }

    uint32_t bits = scalar_int_bits(ins->ty);
    if (bits == 0u) return;

    bool is_cmp = (ins->op >= PIR_OP_CMP_EQ && ins->op <= PIR_OP_CMP_GE_U);
    bool is_arith_imm = (ins->op == PIR_OP_ADD || ins->op == PIR_OP_SUB ||
                         ins->op == PIR_OP_ADD_OVF_S || ins->op == PIR_OP_ADD_OVF_U ||
                         ins->op == PIR_OP_SUB_OVF_S || ins->op == PIR_OP_SUB_OVF_U ||
                         ins->op == PIR_OP_AND || ins->op == PIR_OP_OR || ins->op == PIR_OP_XOR ||
                         ins->op == PIR_OP_SHL || ins->op == PIR_OP_SHR || ins->op == PIR_OP_SAR);
    if (!is_cmp && !is_arith_imm) return;

    uint64_t ca = 0u, cb = 0u;
    bool has_a = (ins->operands[0].kind == PIR_OPER_VAL) && get_int_const(ctx, ins->operands[0], bits, &ca);
    bool has_b = (ins->operands[1].kind == PIR_OPER_VAL) && get_int_const(ctx, ins->operands[1], bits, &cb);

    if (has_b && fits_arm64_imm_for_op(ins->op, cb, bits)) {
        ins->operands[1] = make_canon_imm(cb, bits, ins->ty);
    } else if (has_a && ins->operands[1].kind == PIR_OPER_VAL &&
               fits_arm64_imm_for_op(ins->op, ca, bits)) {
        if (ins->op == PIR_OP_ADD || ins->op == PIR_OP_ADD_OVF_S || ins->op == PIR_OP_ADD_OVF_U ||
            ins->op == PIR_OP_AND || ins->op == PIR_OP_OR  || ins->op == PIR_OP_XOR) {
            ins->operands[0] = ins->operands[1];
            ins->operands[1] = make_canon_imm(ca, bits, ins->ty);
        } else if (is_cmp) {
            PirOpcode flipped;
            if (swap_int_cmp(ins->op, &flipped)) {
                ins->op = flipped;
                ins->operands[0] = ins->operands[1];
                ins->operands[1] = make_canon_imm(ca, bits, ins->ty);
            }
        }
    }
}

/* Returns true if the instruction was completely replaced via alias[] or rewritten to const. */
static bool try_simplify_identity(GvnCtx *ctx, PirInst *ins) {
    PirValueId res = ins->results[0];
    if (res == PIR_INVALID_ID || ins->results[1] != PIR_INVALID_ID) return false;

    /* Redundant trunc(zext(x)) / trunc(sext(x)) -> x, and zext(trunc(x)) -> x when x already fits. */
    if ((ins->op == PIR_OP_TRUNC || ins->op == PIR_OP_ZEXT) &&
        ins->operand_count == 1u && ins->operands[0].kind == PIR_OPER_VAL &&
        ins->ty.kind == PIR_TYPE_SCALAR) {
        uint32_t dst_bits = scalar_int_bits(ins->ty);
        PirValueId sv = ins->operands[0].u.val;
        if (dst_bits > 0u && sv < ctx->nvals) {
            PirInstId did = ctx->fn->values.data[sv].def_inst;
            if (did != PIR_INVALID_ID) {
                const PirInst *d = &ctx->fn->insts.data[did];
                if (ins->op == PIR_OP_TRUNC &&
                    (d->op == PIR_OP_ZEXT || d->op == PIR_OP_SEXT) &&
                    d->operand_count == 1u && d->operands[0].kind == PIR_OPER_VAL &&
                    pir_type_eq(d->operands[0].type, ins->ty)) {
                    ctx->alias[res] = resolve_alias(ctx, d->operands[0].u.val);
                    return true;
                }
                if (ins->op == PIR_OP_ZEXT && d->op == PIR_OP_TRUNC &&
                    d->operand_count == 1u && d->operands[0].kind == PIR_OPER_VAL &&
                    pir_type_eq(d->operands[0].type, ins->ty)) {
                    uint32_t mid_bits = scalar_int_bits(d->ty);
                    PirValueId xv = resolve_alias(ctx, d->operands[0].u.val);
                    if (mid_bits > 0u && mid_bits < dst_bits && xv < ctx->nvals) {
                        PirInstId xid = ctx->fn->values.data[xv].def_inst;
                        if (xid != PIR_INVALID_ID) {
                            const PirInst *x = &ctx->fn->insts.data[xid];
                            uint64_t mid_mask = mask_bits(mid_bits);
                            bool fits = false;
                            if (x->op == PIR_OP_ZEXT && x->operand_count == 1u) {
                                uint32_t xb = scalar_int_bits(x->operands[0].type);
                                if (xb > 0u && xb <= mid_bits) fits = true;
                            } else if (x->op == PIR_OP_AND && x->operand_count == 2u) {
                                uint64_t mc = 0u;
                                if ((get_int_const(ctx, x->operands[1], dst_bits, &mc) && (mc & ~mid_mask) == 0u) ||
                                    (get_int_const(ctx, x->operands[0], dst_bits, &mc) && (mc & ~mid_mask) == 0u)) {
                                    fits = true;
                                }
                            } else if (x->op == PIR_OP_SHR && x->operand_count == 2u) {
                                uint64_t sh = 0u;
                                if (get_int_const(ctx, x->operands[1], dst_bits, &sh) &&
                                    sh >= (uint64_t)(dst_bits - mid_bits) && sh < (uint64_t)dst_bits) {
                                    fits = true;
                                }
                            }
                            if (fits) {
                                ctx->alias[res] = xv;
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    /* Fold cmp_ne(zext(bool), 0) -> bool and cmp_eq(zext(cmp), 0) -> inverted cmp. */
    if ((ins->op == PIR_OP_CMP_NE || ins->op == PIR_OP_CMP_EQ) &&
        ins->operand_count == 2u && ins->ty.kind == PIR_TYPE_SCALAR) {
        uint32_t cbits = scalar_int_bits(ins->ty);
        if (cbits > 0u) {
            uint64_t zc = 0u;
            PirOperand var_op;
            bool has_zero = false;
            if (get_int_const(ctx, ins->operands[1], cbits, &zc) && zc == 0u &&
                ins->operands[0].kind == PIR_OPER_VAL) {
                var_op = ins->operands[0]; has_zero = true;
            } else if (get_int_const(ctx, ins->operands[0], cbits, &zc) && zc == 0u &&
                       ins->operands[1].kind == PIR_OPER_VAL) {
                var_op = ins->operands[1]; has_zero = true;
            }
            if (has_zero) {
                PirValueId bv = var_op.u.val;
                if (bv < ctx->nvals) {
                    PirInstId did = ctx->fn->values.data[bv].def_inst;
                    if (did != PIR_INVALID_ID) {
                        const PirInst *dins = &ctx->fn->insts.data[did];
                        /* Unwrap zext/sext from a 0/1 boolean comparison result */
                        if ((dins->op == PIR_OP_ZEXT || dins->op == PIR_OP_SEXT) &&
                            dins->operand_count == 1u && dins->operands[0].kind == PIR_OPER_VAL) {
                            PirValueId inner = resolve_alias(ctx, dins->operands[0].u.val);
                            if (inner < ctx->nvals) {
                                PirInstId idid = ctx->fn->values.data[inner].def_inst;
                                if (idid != PIR_INVALID_ID &&
                                    ctx->fn->insts.data[idid].ty.kind == PIR_TYPE_SCALAR &&
                                    is_bool_producing_op(ctx->fn->insts.data[idid].op)) {
                                    bv = inner;
                                    did = idid;
                                    dins = &ctx->fn->insts.data[did];
                                }
                            }
                        }
                        if (dins->ty.kind == PIR_TYPE_SCALAR && is_bool_producing_op(dins->op)) {
                            if (ins->op == PIR_OP_CMP_NE) {
                                ctx->alias[res] = bv;
                                return true;
                            }
                            PirOpcode inv;
                            if (invert_int_cmp(dins->op, &inv)) {
                                ins->op            = inv;
                                ins->ty            = dins->ty;
                                ins->flags         = dins->flags;
                                ins->operands      = pir_alloc_ops(ctx->mod, 2u);
                                ins->operands[0]   = dins->operands[0];
                                ins->operands[1]   = dins->operands[1];
                                ins->operand_count = 2u;
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }

    /* select cond, t, f */
    if (ins->op == PIR_OP_SELECT && ins->operand_count == 3u) {
        uint64_t cv = 0u;
        PirOperand chosen;
        bool has_chosen = false;
        if (get_int_const(ctx, ins->operands[0], 8u, &cv) && cv <= 1u) {
            chosen = (cv != 0u) ? ins->operands[1] : ins->operands[2];
            has_chosen = true;
        } else if (pir_operand_eq_exact(ins->operands[1], ins->operands[2]) &&
                   pir_operand_is_proven_bool_i8(ctx->fn, ins->operands[0])) {
            chosen = ins->operands[1];
            has_chosen = true;
        }
        if (has_chosen) {
            if (chosen.kind == PIR_OPER_VAL) {
                ctx->alias[res] = chosen.u.val;
                return true;
            } else if (chosen.kind == PIR_OPER_IMM_INT) {
                uint32_t sbits = scalar_int_bits(ins->ty);
                if (sbits == 0u) return false;
                uint64_t v = (chosen.u.imm_int.is_negative ? (0u - chosen.u.imm_int.lo) : chosen.u.imm_int.lo)
                             & mask_bits(sbits);
                rewrite_to_const_int(ctx, ins, v, ins->ty);
                return true;
            }
        }
        return false;
    }

    /* ptr_add p, 0 -> p (only without [inbounds], since ptr_add [inbounds] null, 0 is UB;
     * and only on 64-bit targets, since 32-bit ptr_add wraps non-canonical 64-bit sym offsets). */
    if (ins->op == PIR_OP_PTR_ADD && ins->operand_count == 2u &&
        (ins->flags & PIR_FLAG_INBOUNDS) == 0u &&
        ctx->target->ptr_bytes != 4u) {
        uint32_t pbits = scalar_int_bits(ins->ty);
        if (pbits == 0u) pbits = (uint32_t)ctx->target->ptr_bytes * 8u;
        uint64_t off = 0u;
        if (ins->operands[0].kind == PIR_OPER_VAL &&
            get_int_const(ctx, ins->operands[1], pbits, &off) &&
            off == 0u) {
            ctx->alias[res] = ins->operands[0].u.val;
            return true;
        }
        return false;
    }

    uint32_t bits = scalar_int_bits(ins->ty);
    if (bits == 0u || ins->operand_count != 2u) return false;

    PirOperand a = ins->operands[0];
    PirOperand b = ins->operands[1];
    uint64_t ca = 0u, cb = 0u;
    bool has_a = get_int_const(ctx, a, bits, &ca);
    bool has_b = get_int_const(ctx, b, bits, &cb);

    /* Same-operand identities */
    if (pir_operand_eq_exact(a, b)) {
        if (ins->op == PIR_OP_XOR || ins->op == PIR_OP_SUB) {
            rewrite_to_const_int(ctx, ins, 0u, ins->ty);
            return true;
        }
        if ((ins->op == PIR_OP_AND || ins->op == PIR_OP_OR) && a.kind == PIR_OPER_VAL) {
            ctx->alias[res] = a.u.val;
            return true;
        }
    }

    /* x op 0 / 0 op x */
    if (ins->op == PIR_OP_ADD || ins->op == PIR_OP_OR || ins->op == PIR_OP_XOR) {
        if (has_b && cb == 0u && a.kind == PIR_OPER_VAL) { ctx->alias[res] = a.u.val; return true; }
        if (has_a && ca == 0u && b.kind == PIR_OPER_VAL) { ctx->alias[res] = b.u.val; return true; }
    }
    if (ins->op == PIR_OP_SUB || ins->op == PIR_OP_SHL || ins->op == PIR_OP_SHR || ins->op == PIR_OP_SAR) {
        if (has_b && cb == 0u && a.kind == PIR_OPER_VAL) { ctx->alias[res] = a.u.val; return true; }
    }
    if (ins->op == PIR_OP_AND) {
        if ((has_b && cb == 0u) || (has_a && ca == 0u)) {
            rewrite_to_const_int(ctx, ins, 0u, ins->ty);
            return true;
        }
        /* Redundant mask after zext: and(zext.i8/i16(x), mask) -> zext when mask covers all low bits. */
        PirOperand vop = a;
        uint64_t mval = 0u;
        bool has_mask = false;
        if (has_b && a.kind == PIR_OPER_VAL) { vop = a; mval = cb; has_mask = true; }
        else if (has_a && b.kind == PIR_OPER_VAL) { vop = b; mval = ca; has_mask = true; }
        if (has_mask && vop.u.val < ctx->nvals) {
            PirInstId did = ctx->fn->values.data[vop.u.val].def_inst;
            if (did != PIR_INVALID_ID) {
                const PirInst *d = &ctx->fn->insts.data[did];
                if (d->op == PIR_OP_ZEXT && d->operand_count == 1u) {
                    uint32_t sb = scalar_int_bits(d->operands[0].type);
                    if (sb > 0u && sb < bits && (mval & mask_bits(sb)) == mask_bits(sb)) {
                        ctx->alias[res] = vop.u.val;
                        return true;
                    }
                }
            }
        }
    }
    if (ins->op == PIR_OP_MUL) {
        if ((has_b && cb == 0u) || (has_a && ca == 0u)) {
            rewrite_to_const_int(ctx, ins, 0u, ins->ty);
            return true;
        }
        if (has_b && cb == 1u && a.kind == PIR_OPER_VAL) { ctx->alias[res] = a.u.val; return true; }
        if (has_a && ca == 1u && b.kind == PIR_OPER_VAL) { ctx->alias[res] = b.u.val; return true; }

        /* Strength reduction: mul x, 2^k -> shl x, k [mask] (only when non-trapping/non-UB). */
        if (!has_ovf_flag(ins->flags)) {
            uint64_t pow_c = 0u;
            PirOperand var_op;
            bool ok = false;
            if (has_b && cb >= 2u && (cb & (cb - 1u)) == 0u && a.kind == PIR_OPER_VAL) {
                pow_c = cb; var_op = a; ok = true;
            } else if (has_a && ca >= 2u && (ca & (ca - 1u)) == 0u && b.kind == PIR_OPER_VAL) {
                pow_c = ca; var_op = b; ok = true;
            }
            if (ok) {
                uint32_t sh = 0u;
                while ((pow_c >> sh) > 1u) sh++;
                if (sh < bits) {
                    ins->op          = PIR_OP_SHL;
                    ins->flags       = PIR_FLAG_MASK;
                    ins->operands[0] = var_op;
                    ins->operands[1] = pir_op_imm_i(sh, false, ins->ty);
                    return false; /* Let GVN number the resulting shl! */
                }
            }
        }
    }
    return false;
}

static bool entry_matches(const GvnEntry *e, const PirInst *ins, const PirOperand *canon_ops) {
    if (e->op != ins->op || !pir_type_eq(e->ty, ins->ty) || e->flags != ins->flags ||
        e->imm_u64 != ins->imm_u64 || e->op_count != ins->operand_count) {
        return false;
    }
    for (uint32_t i = 0u; i < e->op_count; ++i) {
        if (!pir_operand_eq_exact(e->ops[i], canon_ops[i])) return false;
    }
    return true;
}

static void gvn_walk_dom_tree(GvnCtx *ctx, PirBlockId root_b) {
    typedef struct {
        PirBlockId b;
        uint32_t   next_child;
        uint32_t   saved_len;
        bool       entered;
    } GvnFrame;
    PIR_VEC(GvnFrame) stk = {0};
    GvnFrame init = { root_b, 0u, 0u, false };
    PIR_VEC_PUSH(&stk, init);

    while (stk.len > 0u) {
        GvnFrame *fr = &stk.data[stk.len - 1u];
        PirBlockId b = fr->b;
        if (!fr->entered) {
            fr->entered   = true;
            fr->saved_len = ctx->table.len;
            const PirBlock *blk = &ctx->fn->blocks.data[b];

            for (uint32_t j = 0u; j < blk->insts.len; ++j) {
                PirInst *ins = &ctx->fn->insts.data[blk->insts.data[j]];
                resolve_ops(ctx, ins->operands, ins->operand_count);

                if (ins->op == PIR_OP_CONST && ins->results[0] < ctx->nvals) {
                    if (ins->operands[0].kind == PIR_OPER_IMM_INT) {
                        uint64_t v = ins->operands[0].u.imm_int.is_negative
                                     ? (0u - ins->operands[0].u.imm_int.lo)
                                     : ins->operands[0].u.imm_int.lo;
                        ctx->consts[ins->results[0]].is_int = true;
                        ctx->consts[ins->results[0]].bits   = v;
                    }
                }

                if (try_fold_const_int_op(ctx, ins)) {
                    continue;
                }
                if (try_simplify_identity(ctx, ins)) {
                    continue;
                }
                propagate_int_imms(ctx, ins);

                if (!is_gvn_eligible(ctx->mod, ctx->fn, ins)) continue;

                PirOperand canon[3];
                for (uint32_t k = 0u; k < ins->operand_count; ++k) canon[k] = ins->operands[k];
                if (ins->operand_count == 2u && is_commutative(ins) && op_less(canon[1], canon[0])) {
                    PirOperand tmp = canon[0];
                    canon[0] = canon[1];
                    canon[1] = tmp;
                }

                uint32_t h = gvn_hash_inst(ins, canon);
                bool found = false;
                if (ctx->bucket_cap > 0u) {
                    uint32_t bi = h & (ctx->bucket_cap - 1u);
                    for (uint32_t idx = ctx->buckets[bi]; idx != PIR_INVALID_ID;
                         idx = ctx->table.data[idx].prev) {
                        if (ctx->table.data[idx].hash == h &&
                            entry_matches(&ctx->table.data[idx], ins, canon)) {
                            ctx->alias[ins->results[0]] = ctx->table.data[idx].val_id;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    if (ctx->bucket_cap == 0u) {
                        gvn_rehash(ctx, 256u);
                    } else if (ctx->table.len * 2u >= ctx->bucket_cap) {
                        gvn_rehash(ctx, ctx->bucket_cap * 2u);
                    }
                    uint32_t bi = h & (ctx->bucket_cap - 1u);
                    GvnEntry e;
                    memset(&e, 0, sizeof(e));
                    e.op       = ins->op;
                    e.ty       = ins->ty;
                    e.flags    = ins->flags;
                    e.imm_u64  = ins->imm_u64;
                    e.op_count = ins->operand_count;
                    for (uint32_t k = 0u; k < ins->operand_count; ++k) e.ops[k] = canon[k];
                    e.val_id   = ins->results[0];
                    e.hash     = h;
                    e.prev     = ctx->buckets[bi];
                    uint32_t new_idx = ctx->table.len;
                    PIR_VEC_PUSH(&ctx->table, e);
                    ctx->buckets[bi] = new_idx;
                }
            }
        }
        if (fr->next_child < ctx->dom->children[b].len) {
            PirBlockId child = ctx->dom->children[b].data[fr->next_child++];
            GvnFrame cf = { child, 0u, 0u, false };
            PIR_VEC_PUSH(&stk, cf);
        } else {
            uint32_t mask = ctx->bucket_cap > 0u ? (ctx->bucket_cap - 1u) : 0u;
            while (ctx->table.len > fr->saved_len) {
                ctx->table.len--;
                const GvnEntry *e = &ctx->table.data[ctx->table.len];
                if (ctx->bucket_cap > 0u) {
                    ctx->buckets[e->hash & mask] = e->prev;
                }
            }
            stk.len--;
        }
    }
    PIR_VEC_FREE(&stk);
}

void pir_opt_gvn(PirModule *mod, PirFunction *fn, const PirTargetSpec *target) {
    if (fn->blocks.len == 0u || fn->values.len == 0u) return;

    PirCfg cfg;
    PirDom dom;
    pir_cfg_build(&cfg, fn);
    pir_dom_build(&dom, &cfg);

    uint32_t nvals = fn->values.len;
    PirValueId *alias = (PirValueId *)malloc(nvals * sizeof(PirValueId));
    ConstVal *consts  = (ConstVal *)calloc(nvals, sizeof(ConstVal));
    if (!alias || !consts) abort();
    for (uint32_t v = 0u; v < nvals; ++v) alias[v] = PIR_INVALID_ID;

    GvnCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod    = mod;
    ctx.fn     = fn;
    ctx.target = target;
    ctx.dom    = &dom;
    ctx.alias  = alias;
    ctx.consts = consts;
    ctx.nvals  = nvals;

    gvn_walk_dom_tree(&ctx, 0u);

    /* Rewrite all remaining uses across all blocks (including branch target args). */
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            resolve_ops(&ctx, ins->operands, ins->operand_count);
            resolve_ops(&ctx, ins->targets[0].args, ins->targets[0].arg_count);
            resolve_ops(&ctx, ins->targets[1].args, ins->targets[1].arg_count);
            for (uint32_t c = 0u; c < ins->switch_case_count; ++c) {
                resolve_ops(&ctx, ins->switch_cases[c].target.args,
                            ins->switch_cases[c].target.arg_count);
            }
        }
    }

    free(ctx.buckets);
    PIR_VEC_FREE(&ctx.table);
    free(consts);
    free(alias);
    pir_dom_destroy(&dom);
    pir_cfg_destroy(&cfg);
}
