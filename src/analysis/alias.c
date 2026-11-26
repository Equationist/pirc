/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "analysis/alias.h"

#include <string.h>

static uint64_t gcd_u64(uint64_t a, uint64_t b) {
    while (b != 0u) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static int64_t imm_to_i64(const PirOperand *c) {
    uint64_t v = c->u.imm_int.is_negative ? (UINT64_C(0) - c->u.imm_int.lo)
                                          : c->u.imm_int.lo;
    if (c->type.kind == PIR_TYPE_SCALAR && c->type.u.scalar == PIR_SCALAR_I32) {
        return (int64_t)(int32_t)(uint32_t)v;
    }
    return (int64_t)v;
}

static bool get_const_i64(const PirFunction *fn, PirOperand op, int64_t *out) {
    if (op.kind == PIR_OPER_IMM_INT) {
        *out = imm_to_i64(&op);
        return true;
    }
    if (op.kind == PIR_OPER_VAL && op.u.val < fn->values.len) {
        PirInstId did = fn->values.data[op.u.val].def_inst;
        if (did != PIR_INVALID_ID && fn->insts.data[did].op == PIR_OP_CONST &&
            fn->insts.data[did].operand_count == 1u &&
            fn->insts.data[did].operands[0].kind == PIR_OPER_IMM_INT) {
            *out = imm_to_i64(&fn->insts.data[did].operands[0]);
            return true;
        }
    }
    return false;
}

/* Extract constant byte stride factor for a variable ptr_add offset (e.g. mul(idx, 56) or shl(idx, 3)). */
static uint64_t extract_var_stride(const PirFunction *fn, PirOperand off_op) {
    if (off_op.kind != PIR_OPER_VAL || off_op.u.val >= fn->values.len) return 1u;
    PirInstId did = fn->values.data[off_op.u.val].def_inst;
    if (did == PIR_INVALID_ID) return 1u;
    const PirInst *ins = &fn->insts.data[did];
    if (ins->op == PIR_OP_MUL && ins->operand_count == 2u) {
        int64_t c = 0;
        bool found = (get_const_i64(fn, ins->operands[0], &c) && c > 1 && c <= 65536) ||
                     (get_const_i64(fn, ins->operands[1], &c) && c > 1 && c <= 65536);
        if (found) {
            uint64_t u = (uint64_t)c;
            bool is_i64 = (ins->ty.kind == PIR_TYPE_SCALAR && ins->ty.u.scalar == PIR_SCALAR_I64);
            if ((is_i64 && (ins->flags & (PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) != 0u) ||
                (!is_i64 && (ins->flags & PIR_FLAG_UNSAFE_S) != 0u)) {
                return u;
            }
            /* Wrapping mul mod 2^32 / 2^64 only guarantees alignment to the largest pow2 factor of c. */
            return u & (0u - u);
        }
    } else if (ins->op == PIR_OP_SHL && ins->operand_count == 2u) {
        int64_t sh = 0;
        if (get_const_i64(fn, ins->operands[1], &sh) && sh >= 1 && sh <= 16) {
            return (uint64_t)1u << (uint32_t)sh;
        }
    }
    return 1u;
}

PirPtrInfo pir_alias_decompose(const PirFunction *fn, PirOperand op) {
    PirPtrInfo info;
    memset(&info, 0, sizeof(info));
    info.kind       = PIR_PTR_UNKNOWN;
    info.has_offset = true;
    info.offset     = 0;
    info.stride     = 0u;

    for (uint32_t depth = 0u; depth < 32u; ++depth) {
        if (op.kind == PIR_OPER_SYM) {
            info.kind        = PIR_PTR_GLOBAL;
            info.global_name = op.u.sym.name;
            int64_t so = op.u.sym.offset;
            if ((so > 0 && info.offset > INT64_MAX - so) ||
                (so < 0 && info.offset < INT64_MIN - so)) {
                info.has_offset = false;
                info.stride     = 1u;
            } else {
                info.offset += so;
            }
            return info;
        }
        if (op.kind != PIR_OPER_VAL || op.u.val >= fn->values.len) {
            info.kind = PIR_PTR_UNKNOWN;
            return info;
        }
        const PirValueDef *vd = &fn->values.data[op.u.val];
        if (vd->def_block == PIR_INVALID_ID) {
            /* Function parameter */
            uint32_t pi = vd->param_index;
            info.obj_id = pi;
            info.kind   = (pi < fn->param_count && (fn->params[pi].attrs & PIR_PATTR_NOALIAS))
                          ? PIR_PTR_PARAM_NOALIAS : PIR_PTR_PARAM;
            return info;
        }
        if (vd->def_inst == PIR_INVALID_ID) {
            info.kind = PIR_PTR_UNKNOWN;
            return info;
        }
        const PirInst *ins = &fn->insts.data[vd->def_inst];
        if (ins->op == PIR_OP_ALLOCA_PTR || ins->op == PIR_OP_ALLOCA_DYN) {
            info.kind   = PIR_PTR_ALLOCA;
            info.obj_id = vd->def_inst;
            return info;
        }
        if (ins->op == PIR_OP_ADDR || ins->op == PIR_OP_CONST) {
            op = ins->operands[0];
            continue;
        }
        if (ins->op == PIR_OP_PTR_ADD) {
            int64_t d = 0;
            if (get_const_i64(fn, ins->operands[1], &d)) {
                if ((d > 0 && info.offset > INT64_MAX - d) ||
                    (d < 0 && info.offset < INT64_MIN - d)) {
                    info.has_offset = false;
                    info.stride     = 1u;
                } else {
                    info.offset += d;
                }
            } else {
                uint64_t st = extract_var_stride(fn, ins->operands[1]);
                info.stride = info.has_offset ? st : gcd_u64(info.stride, st);
                info.has_offset = false;
            }
            op = ins->operands[0];
            continue;
        }
        info.kind = PIR_PTR_UNKNOWN;
        return info;
    }
    info.kind = PIR_PTR_UNKNOWN;
    return info;
}

static bool same_base(const PirPtrInfo *a, const PirPtrInfo *b) {
    if (a->kind == PIR_PTR_UNKNOWN || a->kind != b->kind) return false;
    if (a->kind == PIR_PTR_GLOBAL) {
        return a->global_name && b->global_name && strcmp(a->global_name, b->global_name) == 0;
    }
    return a->obj_id == b->obj_id;
}

PirAliasResult pir_alias_query(const PirFunction *fn,
                               PirOperand p1, uint32_t sz1,
                               PirOperand p2, uint32_t sz2) {
    /* Exact same SSA value -> MUST alias if equal size. */
    if (p1.kind == PIR_OPER_VAL && p2.kind == PIR_OPER_VAL && p1.u.val == p2.u.val) {
        return (sz1 == sz2) ? PIR_ALIAS_MUST : PIR_ALIAS_MAY;
    }

    PirPtrInfo a = pir_alias_decompose(fn, p1);
    PirPtrInfo b = pir_alias_decompose(fn, p2);

    if (same_base(&a, &b)) {
        if (a.has_offset && b.has_offset) {
            if (a.offset == b.offset && sz1 == sz2) return PIR_ALIAS_MUST;
            if (a.offset < b.offset &&
                (uint64_t)b.offset - (uint64_t)a.offset >= (uint64_t)sz1) {
                return PIR_ALIAS_NO;
            }
            if (b.offset < a.offset &&
                (uint64_t)a.offset - (uint64_t)b.offset >= (uint64_t)sz2) {
                return PIR_ALIAS_NO;
            }
            return PIR_ALIAS_MAY;
        }
        /* Stride-modular field disambiguation for arrays of structs:
         * base + S*i + off1 (sz1) and base + S*j + off2 (sz2) can never alias if
         * [off1 mod S, off1 mod S + sz1) and [off2 mod S, off2 mod S + sz2) are disjoint within [0, S). */
        uint64_t stride = a.has_offset ? b.stride
                        : b.has_offset ? a.stride
                        : gcd_u64(a.stride, b.stride);
        if (stride > 1u && sz1 > 0u && sz2 > 0u && sz1 <= stride && sz2 <= stride) {
            int64_t s  = (int64_t)stride;
            int64_t oa = ((a.offset % s) + s) % s;
            int64_t ob = ((b.offset % s) + s) % s;
            if (oa + (int64_t)sz1 <= s && ob + (int64_t)sz2 <= s) {
                if (oa + (int64_t)sz1 <= ob || ob + (int64_t)sz2 <= oa) {
                    return PIR_ALIAS_NO;
                }
            }
        }
        return PIR_ALIAS_MAY;
    }

    /* Distinct known objects never alias. */
    if (a.kind == PIR_PTR_ALLOCA && b.kind == PIR_PTR_ALLOCA) return PIR_ALIAS_NO;
    if ((a.kind == PIR_PTR_ALLOCA && b.kind == PIR_PTR_GLOBAL) ||
        (a.kind == PIR_PTR_GLOBAL && b.kind == PIR_PTR_ALLOCA)) return PIR_ALIAS_NO;
    if (a.kind == PIR_PTR_GLOBAL && b.kind == PIR_PTR_GLOBAL) return PIR_ALIAS_NO;

    /* noalias parameter cannot alias any other base object (alloca, global, other param). */
    if ((a.kind == PIR_PTR_PARAM_NOALIAS && b.kind != PIR_PTR_UNKNOWN) ||
        (b.kind == PIR_PTR_PARAM_NOALIAS && a.kind != PIR_PTR_UNKNOWN)) {
        return PIR_ALIAS_NO;
    }

    return PIR_ALIAS_MAY;
}
