/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lower/lower.h"

#include <stdlib.h>
#include <string.h>

static uint64_t align_up_u64_chk(uint64_t v, uint32_t align, bool *ovf) {
    if (align <= 1u) return v;
    uint64_t m = (uint64_t)align - 1u;
    if (v > UINT64_MAX - m) {
        if (ovf) *ovf = true;
        return UINT64_MAX;
    }
    return (v + m) & ~m;
}

static uint64_t align_up_u64(uint64_t v, uint32_t align) {
    return align_up_u64_chk(v, align, NULL);
}

static bool is_pow2_u64(uint64_t v, uint32_t *out_shift) {
    if (v == 0u || (v & (v - 1u)) != 0u) return false;
    uint32_t s = 0u;
    while (v > 1u) { v >>= 1u; s++; }
    *out_shift = s;
    return true;
}

PirTypeLayout pir_type_layout(const PirModule *mod, const PirTargetSpec *target, PirType ty) {
    PirTypeLayout res = { 0u, 1u, 0u, false };
    switch (ty.kind) {
        case PIR_TYPE_SCALAR:
            switch (ty.u.scalar) {
                case PIR_SCALAR_VOID:   res.size = 0u; res.align = 1u; break;
                case PIR_SCALAR_I8:     res.size = 1u; res.align = 1u; break;
                case PIR_SCALAR_I16:    res.size = 2u; res.align = 2u; break;
                case PIR_SCALAR_I32:
                case PIR_SCALAR_F32:    res.size = 4u; res.align = 4u; break;
                case PIR_SCALAR_I64:
                case PIR_SCALAR_F64:    res.size = 8u; res.align = 8u; break;
                case PIR_SCALAR_I128:   res.size = 16u; res.align = 16u; break;
                case PIR_SCALAR_PTR:
                    res.size  = target->ptr_bytes;
                    res.align = target->ptr_bytes;
                    break;
                case PIR_SCALAR_VALIST:
                    res.size  = target->va_list_size;
                    res.align = target->va_list_align;
                    break;
            }
            return res;
        case PIR_TYPE_VECTOR: {
            uint32_t b = (uint32_t)(pir_vec_bits(ty.u.vec) / 8u);
            res.size  = b;
            res.align = b;
            return res;
        }
        case PIR_TYPE_NAMED_AGG: {
            const PirAggDef *agg = &mod->agg_types.data[ty.u.agg_id];
            switch (agg->kind) {
                case PIR_AGG_STRUCT: {
                    uint64_t off = 0u;
                    uint32_t max_al = 1u;
                    for (uint32_t i = 0u; i < agg->field_count; ++i) {
                        PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[i]);
                        if (fl.overflow) res.overflow = true;
                        off = align_up_u64_chk(off, fl.align, &res.overflow);
                        if (fl.size > UINT64_MAX - off) {
                            res.overflow = true;
                            off = UINT64_MAX;
                        } else {
                            off += fl.size;
                        }
                        if (fl.align > max_al) max_al = fl.align;
                    }
                    res.size  = align_up_u64_chk(off, max_al, &res.overflow);
                    res.align = max_al;
                    break;
                }
                case PIR_AGG_PACKED_STRUCT: {
                    uint64_t off = 0u;
                    for (uint32_t i = 0u; i < agg->field_count; ++i) {
                        PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[i]);
                        if (fl.overflow) res.overflow = true;
                        if (fl.size > UINT64_MAX - off) {
                            res.overflow = true;
                            off = UINT64_MAX;
                        } else {
                            off += fl.size;
                        }
                    }
                    res.size  = off;
                    res.align = 1u;
                    break;
                }
                case PIR_AGG_UNION: {
                    uint64_t max_sz = 0u;
                    uint32_t max_al = 1u;
                    for (uint32_t i = 0u; i < agg->field_count; ++i) {
                        PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[i]);
                        if (fl.overflow) res.overflow = true;
                        if (fl.size > max_sz) max_sz = fl.size;
                        if (fl.align > max_al) max_al = fl.align;
                    }
                    res.size  = align_up_u64_chk(max_sz, max_al, &res.overflow);
                    res.align = max_al;
                    break;
                }
                case PIR_AGG_ARRAY: {
                    PirTypeLayout el = pir_type_layout(mod, target, agg->elem_type);
                    if (el.overflow) res.overflow = true;
                    uint64_t stride  = align_up_u64_chk(el.size, el.align, &res.overflow);
                    if (stride > 0u && agg->array_len > UINT64_MAX / stride) {
                        res.overflow = true;
                        res.size = UINT64_MAX;
                    } else {
                        res.size = stride * agg->array_len;
                    }
                    res.align  = el.align;
                    res.stride = stride;
                    break;
                }
                case PIR_AGG_PAD:
                    res.size  = agg->array_len;
                    res.align = 1u;
                    break;
            }
            if (agg->min_align > res.align) {
                res.align = agg->min_align;
                res.size  = align_up_u64_chk(res.size, res.align, &res.overflow);
            }
            return res;
        }
    }
    return res;
}

uint64_t pir_field_offset(const PirModule *mod, const PirTargetSpec *target,
                          PirTypeId agg_id, uint32_t field_idx) {
    const PirAggDef *agg = &mod->agg_types.data[agg_id];
    if (agg->kind == PIR_AGG_UNION || agg->kind == PIR_AGG_ARRAY) {
        return 0u;
    }
    uint64_t off = 0u;
    for (uint32_t i = 0u; i < field_idx; ++i) {
        PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[i]);
        if (agg->kind == PIR_AGG_STRUCT) {
            off = align_up_u64(off, fl.align);
        }
        off += fl.size;
    }
    if (agg->kind == PIR_AGG_STRUCT && field_idx < agg->field_count) {
        PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[field_idx]);
        off = align_up_u64(off, fl.align);
    }
    return off;
}

static PirValueId emit_ptr_add_const(PirModule *mod, PirFunction *fn, PirBlockId blk_id,
                                     PirInstIdVec *out, PirOperand base, uint64_t offset,
                                     PirType idx_ty, uint32_t line, uint32_t loc) {
    if (offset == 0u && base.kind == PIR_OPER_VAL) {
        return base.u.val;
    }
    PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, idx_ty, line, loc);
    pa.flags         = PIR_FLAG_INBOUNDS;
    pa.operand_count = 2u;
    pa.operands      = pir_alloc_ops(mod, 2u);
    pa.operands[0]   = base;
    pa.operands[1]   = pir_op_imm_i(offset, false, idx_ty);
    pa.results[0]    = pir_fn_new_val(mod, fn, "gep", pir_type_scalar(PIR_SCALAR_PTR),
                                      blk_id, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, pa);
    return pa.results[0];
}

static PirScalarType pick_chunk_type(uint64_t rem, uint32_t align, uint32_t *out_bytes) {
    if (rem >= 8u && align >= 8u) { *out_bytes = 8u; return PIR_SCALAR_I64; }
    if (rem >= 4u && align >= 4u) { *out_bytes = 4u; return PIR_SCALAR_I32; }
    if (rem >= 2u && align >= 2u) { *out_bytes = 2u; return PIR_SCALAR_I16; }
    *out_bytes = 1u;
    return PIR_SCALAR_I8;
}

static void lower_mem_intrinsic(PirModule *mod, const PirTargetSpec *target,
                                PirFuncId fi, PirBlockId blk_id, PirInstIdVec *out,
                                const PirInst *inst) {
    PirFunction *fn = &mod->funcs.data[fi];
    PirType idx_ty = pir_type_scalar(target->ptr_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);
    PirOperand dst = inst->operands[0];
    PirOperand src = inst->operands[1];
    PirOperand len = inst->operands[2];
    uint32_t al = inst->align;

    bool const_small = (len.kind == PIR_OPER_IMM_INT && !len.u.imm_int.is_negative &&
                        len.u.imm_int.hi == 0u && len.u.imm_int.lo <= 64u);

    if (const_small && (inst->op == PIR_OP_MEMCPY || inst->op == PIR_OP_MEMMOVE)) {
        uint64_t total = len.u.imm_int.lo;
        /* Load all chunks into SSA temps before storing so overlapping memmove is exact. */
        PirValueId vals[64];
        PirScalarType ctys[64];
        uint64_t offs[64];
        uint32_t caligns[64];
        uint32_t nchunks = 0u;
        uint64_t off = 0u;
        while (off < total) {
            uint32_t csz = 1u;
            PirScalarType ct = pick_chunk_type(total - off, al, &csz);
            PirValueId sp = emit_ptr_add_const(mod, fn, blk_id, out, src, off, idx_ty,
                                               inst->src_line, inst->loc_idx);
            PirInst ld = pir_make_inst(PIR_OP_LOAD, pir_type_scalar(ct), inst->src_line, inst->loc_idx);
            ld.align         = csz;
            ld.operand_count = 1u;
            ld.operands      = pir_alloc_ops(mod, 1u);
            ld.operands[0]   = pir_op_val(sp, pir_type_scalar(PIR_SCALAR_PTR));
            ld.results[0]    = pir_fn_new_val(mod, fn, "mc", pir_type_scalar(ct),
                                              blk_id, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, out, ld);
            vals[nchunks]    = ld.results[0];
            ctys[nchunks]    = ct;
            offs[nchunks]    = off;
            caligns[nchunks] = csz;
            nchunks++;
            off += csz;
        }
        for (uint32_t i = 0u; i < nchunks; ++i) {
            PirValueId dp = emit_ptr_add_const(mod, fn, blk_id, out, dst, offs[i], idx_ty,
                                               inst->src_line, inst->loc_idx);
            PirInst st = pir_make_inst(PIR_OP_STORE, pir_type_scalar(ctys[i]),
                                       inst->src_line, inst->loc_idx);
            st.align         = caligns[i];
            st.operand_count = 2u;
            st.operands      = pir_alloc_ops(mod, 2u);
            st.operands[0]   = pir_op_val(vals[i], pir_type_scalar(ctys[i]));
            st.operands[1]   = pir_op_val(dp, pir_type_scalar(PIR_SCALAR_PTR));
            pir_fn_append_inst(fn, out, st);
        }
        return;
    }

    if (const_small && inst->op == PIR_OP_MEMSET && src.kind == PIR_OPER_IMM_INT) {
        uint64_t total = len.u.imm_int.lo;
        uint8_t byte_v = (uint8_t)(src.u.imm_int.is_negative
                                   ? (UINT64_C(0) - src.u.imm_int.lo)
                                   : src.u.imm_int.lo);
        uint64_t rep64 = (uint64_t)byte_v * 0x0101010101010101ULL;
        uint64_t off = 0u;
        while (off < total) {
            uint32_t csz = 1u;
            PirScalarType ct = pick_chunk_type(total - off, al, &csz);
            PirValueId dp = emit_ptr_add_const(mod, fn, blk_id, out, dst, off, idx_ty,
                                               inst->src_line, inst->loc_idx);
            uint64_t val = (csz == 8u) ? rep64 : (rep64 & (((uint64_t)1u << (csz * 8u)) - 1u));
            PirInst st = pir_make_inst(PIR_OP_STORE, pir_type_scalar(ct),
                                       inst->src_line, inst->loc_idx);
            st.align         = csz;
            st.operand_count = 2u;
            st.operands      = pir_alloc_ops(mod, 2u);
            st.operands[0]   = pir_op_imm_i(val, false, pir_type_scalar(ct));
            st.operands[1]   = pir_op_val(dp, pir_type_scalar(PIR_SCALAR_PTR));
            pir_fn_append_inst(fn, out, st);
            off += csz;
        }
        return;
    }

    /* Large or dynamic length -> extern C call (@memcpy / @memmove / @memset). */
    const char *cname = (inst->op == PIR_OP_MEMCPY)  ? "memcpy"
                      : (inst->op == PIR_OP_MEMMOVE) ? "memmove" : "memset";
    PirType ptys[3];
    ptys[0] = pir_type_scalar(PIR_SCALAR_PTR);
    ptys[1] = (inst->op == PIR_OP_MEMSET) ? pir_type_scalar(PIR_SCALAR_I32)
                                          : pir_type_scalar(PIR_SCALAR_PTR);
    ptys[2] = idx_ty;
    PirFuncId cid = pir_ensure_extern_func(mod, cname, ptys, 3u, pir_type_scalar(PIR_SCALAR_PTR));
    fn = &mod->funcs.data[fi]; /* Re-fetch after potential mod->funcs realloc */

    PirOperand arg1 = src;
    if (inst->op == PIR_OP_MEMSET) {
        if (src.kind == PIR_OPER_IMM_INT) {
            arg1.type = pir_type_scalar(PIR_SCALAR_I32);
        } else {
            PirInst z = pir_make_inst(PIR_OP_ZEXT, pir_type_scalar(PIR_SCALAR_I32),
                                      inst->src_line, inst->loc_idx);
            z.operand_count = 1u;
            z.operands      = pir_alloc_ops(mod, 1u);
            z.operands[0]   = src;
            z.results[0]    = pir_fn_new_val(mod, fn, "ms_b", pir_type_scalar(PIR_SCALAR_I32),
                                             blk_id, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, out, z);
            arg1 = pir_op_val(z.results[0], pir_type_scalar(PIR_SCALAR_I32));
        }
    }

    PirOperand arg2 = len;
    if (!pir_type_eq(arg2.type, idx_ty)) {
        if (arg2.kind == PIR_OPER_IMM_INT) {
            arg2.type = idx_ty;
        } else {
            PirOpcode cop = (idx_ty.u.scalar == PIR_SCALAR_I64) ? PIR_OP_ZEXT : PIR_OP_TRUNC;
            PirInst c = pir_make_inst(cop, idx_ty, inst->src_line, inst->loc_idx);
            c.operand_count = 1u;
            c.operands      = pir_alloc_ops(mod, 1u);
            c.operands[0]   = len;
            c.results[0]    = pir_fn_new_val(mod, fn, "mlen", idx_ty, blk_id, PIR_INVALID_ID, 0u);
            pir_fn_append_inst(fn, out, c);
            arg2 = pir_op_val(c.results[0], idx_ty);
        }
    }

    PirInst call = pir_make_inst(PIR_OP_CALL, pir_type_scalar(PIR_SCALAR_PTR),
                                 inst->src_line, inst->loc_idx);
    call.flags         = PIR_FLAG_NOUNWIND;
    call.operand_count = 4u;
    call.operands      = pir_alloc_ops(mod, 4u);
    call.operands[0]   = pir_op_sym_func(mod->funcs.data[cid].name, cid);
    call.operands[1]   = dst;
    call.operands[2]   = arg1;
    call.operands[3]   = arg2;
    pir_fn_append_inst(fn, out, call);
}

typedef PIR_VEC(PirGlobalItem) GItemVec;

static void emit_zero_gitem(GItemVec *out, uint64_t len) {
    if (len == 0u) return;
    if (out->len > 0u && out->data[out->len - 1u].kind == PIR_GITEM_ZERO) {
        out->data[out->len - 1u].len += len;
        return;
    }
    PirGlobalItem zi;
    memset(&zi, 0, sizeof(zi));
    zi.kind = PIR_GITEM_ZERO;
    zi.len  = len;
    PIR_VEC_PUSH(out, zi);
}

static void emit_bytes_gitem(PirModule *mod, GItemVec *out, const uint8_t *buf, uint64_t len) {
    if (len == 0u) return;
    uint8_t *copy = PIR_ARENA_NEW_ARRAY(&mod->arena, uint8_t, len);
    memcpy(copy, buf, (size_t)len);
    PirGlobalItem bi;
    memset(&bi, 0, sizeof(bi));
    bi.kind  = PIR_GITEM_BYTES;
    bi.bytes = copy;
    bi.len   = len;
    PIR_VEC_PUSH(out, bi);
}

static void encode_u64_bytes(uint8_t *buf, uint64_t val, uint32_t sz, bool be) {
    for (uint32_t i = 0u; i < sz; ++i) {
        uint32_t sh = be ? ((sz - 1u - i) * 8u) : (i * 8u);
        buf[i] = (uint8_t)((val >> sh) & 0xFFu);
    }
}

static void flatten_agg_global(PirModule *mod, const PirTargetSpec *target, PirType ty,
                               const PirGlobalItem *src, uint32_t src_cnt, uint32_t *idx,
                               GItemVec *out) {
    PirTypeLayout ly = pir_type_layout(mod, target, ty);
    if (ly.size == 0u) return;
    if (*idx >= src_cnt) {
        emit_zero_gitem(out, ly.size);
        return;
    }
    if (ty.kind == PIR_TYPE_SCALAR) {
        const PirGlobalItem *it = &src[(*idx)++];
        if (it->kind == PIR_GITEM_ZERO) {
            emit_zero_gitem(out, ly.size);
        } else if (it->kind == PIR_GITEM_RELOC) {
            PIR_VEC_PUSH(out, *it);
        } else if (it->kind == PIR_GITEM_BYTES) {
            uint64_t copy_n = (it->len < ly.size) ? it->len : ly.size;
            emit_bytes_gitem(mod, out, it->bytes, copy_n);
            if (copy_n < ly.size) emit_zero_gitem(out, ly.size - copy_n);
        } else if (it->kind == PIR_GITEM_FLOAT) {
            if (ty.u.scalar == PIR_SCALAR_F32) {
                double dv = 0.0;
                memcpy(&dv, &it->float_bits, 8u);
                float fv = (float)dv;
                uint32_t fb = 0u;
                memcpy(&fb, &fv, 4u);
                uint8_t buf[4];
                encode_u64_bytes(buf, (uint64_t)fb, 4u, target->is_big_endian);
                emit_bytes_gitem(mod, out, buf, 4u);
            } else {
                uint8_t buf[8];
                encode_u64_bytes(buf, it->float_bits, 8u, target->is_big_endian);
                emit_bytes_gitem(mod, out, buf, 8u);
            }
        } else if (it->kind == PIR_GITEM_INT) {
            uint64_t lo = it->int_lo;
            uint64_t hi = it->int_hi;
            if (it->int_neg) {
                uint64_t nlo = ~lo + 1u;
                uint64_t cy  = (nlo == 0u) ? 1u : 0u;
                lo = nlo;
                hi = ~hi + cy;
            }
            if (ty.u.scalar == PIR_SCALAR_I128) {
                uint8_t buf[16];
                if (target->is_big_endian) {
                    encode_u64_bytes(buf, hi, 8u, true);
                    encode_u64_bytes(buf + 8, lo, 8u, true);
                } else {
                    encode_u64_bytes(buf, lo, 8u, false);
                    encode_u64_bytes(buf + 8, hi, 8u, false);
                }
                emit_bytes_gitem(mod, out, buf, 16u);
            } else if (ty.u.scalar == PIR_SCALAR_F32) {
                float fv = it->int_neg ? -(float)it->int_lo : (float)it->int_lo;
                uint32_t fb = 0u;
                memcpy(&fb, &fv, 4u);
                uint8_t buf[4];
                encode_u64_bytes(buf, (uint64_t)fb, 4u, target->is_big_endian);
                emit_bytes_gitem(mod, out, buf, 4u);
            } else if (ty.u.scalar == PIR_SCALAR_F64) {
                double dv = it->int_neg ? -(double)it->int_lo : (double)it->int_lo;
                uint64_t db = 0u;
                memcpy(&db, &dv, 8u);
                uint8_t buf[8];
                encode_u64_bytes(buf, db, 8u, target->is_big_endian);
                emit_bytes_gitem(mod, out, buf, 8u);
            } else {
                uint8_t buf[8];
                encode_u64_bytes(buf, lo, (uint32_t)ly.size, target->is_big_endian);
                emit_bytes_gitem(mod, out, buf, ly.size);
            }
        }
        return;
    }
    if (ty.kind == PIR_TYPE_VECTOR) {
        PirType ety = pir_type_scalar(pir_vec_elem_scalar(ty.u.vec));
        uint32_t lanes = pir_vec_lanes(ty.u.vec);
        for (uint32_t l = 0u; l < lanes; ++l) {
            flatten_agg_global(mod, target, ety, src, src_cnt, idx, out);
        }
        return;
    }
    const PirAggDef *agg = &mod->agg_types.data[ty.u.agg_id];
    switch (agg->kind) {
        case PIR_AGG_PAD:
            emit_zero_gitem(out, ly.size);
            break;
        case PIR_AGG_ARRAY:
            if (*idx < src_cnt && src[*idx].kind == PIR_GITEM_BYTES &&
                agg->elem_type.kind == PIR_TYPE_SCALAR &&
                agg->elem_type.u.scalar == PIR_SCALAR_I8) {
                const PirGlobalItem *bi = &src[(*idx)++];
                uint64_t cn = (bi->len < ly.size) ? bi->len : ly.size;
                emit_bytes_gitem(mod, out, bi->bytes, cn);
                if (cn < ly.size) emit_zero_gitem(out, ly.size - cn);
            } else {
                PirTypeLayout el = pir_type_layout(mod, target, agg->elem_type);
                for (uint64_t i = 0u; i < agg->array_len; ++i) {
                    flatten_agg_global(mod, target, agg->elem_type, src, src_cnt, idx, out);
                    if (ly.stride > el.size) emit_zero_gitem(out, ly.stride - el.size);
                }
            }
            break;
        case PIR_AGG_STRUCT:
        case PIR_AGG_PACKED_STRUCT: {
            uint64_t off = 0u;
            for (uint32_t f = 0u; f < agg->field_count; ++f) {
                PirTypeLayout fl = pir_type_layout(mod, target, agg->fields[f]);
                if (agg->kind == PIR_AGG_STRUCT) {
                    uint64_t next_off = align_up_u64(off, fl.align);
                    if (next_off > off) emit_zero_gitem(out, next_off - off);
                    off = next_off;
                }
                flatten_agg_global(mod, target, agg->fields[f], src, src_cnt, idx, out);
                off += fl.size;
            }
            if (ly.size > off) emit_zero_gitem(out, ly.size - off);
            break;
        }
        case PIR_AGG_UNION:
            if (agg->field_count > 0u) {
                PirTypeLayout f0 = pir_type_layout(mod, target, agg->fields[0]);
                flatten_agg_global(mod, target, agg->fields[0], src, src_cnt, idx, out);
                if (ly.size > f0.size) emit_zero_gitem(out, ly.size - f0.size);
            } else {
                emit_zero_gitem(out, ly.size);
            }
            break;
    }
}

bool pir_lower_aggregates(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    PirType ptr_int_ty = pir_type_scalar(target->ptr_bytes == 4u ? PIR_SCALAR_I32 : PIR_SCALAR_I64);

    /* Validate aggregate type layouts for 64-bit size overflow before lowering. */
    for (uint32_t ti = 0u; ti < mod->agg_types.len; ++ti) {
        PirTypeLayout ly = pir_type_layout(mod, target, pir_type_agg(ti));
        if (ly.overflow) {
            pir_diag_error(diag, mod->agg_types.data[ti].src_line, 0u,
                           "aggregate type layout size overflow");
            return false;
        }
    }

    /* 1. Lower aggregate/memory instructions in every defined function. */
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        if (!mod->funcs.data[fi].is_defined) continue;

        /* Note: lower_mem_intrinsic may append extern funcs and reallocate mod->funcs.data,
         * so index mod->funcs.data[fi] fresh when needed. */
        uint32_t num_blocks = mod->funcs.data[fi].blocks.len;
        for (uint32_t b = 0u; b < num_blocks; ++b) {
            PirInstIdVec new_insts = {0};
            uint32_t old_len = mod->funcs.data[fi].blocks.data[b].insts.len;

            for (uint32_t j = 0u; j < old_len; ++j) {
                PirInstId iid = mod->funcs.data[fi].blocks.data[b].insts.data[j];
                PirInst copy  = mod->funcs.data[fi].insts.data[iid];
                PirFunction *fn = &mod->funcs.data[fi];

                if (copy.op == PIR_OP_ALLOCA) {
                    PirTypeLayout ly = pir_type_layout(mod, target, copy.aux_type);
                    if (ly.overflow) {
                        pir_diag_error(diag, copy.src_line, 0u, "alloca type size overflow");
                        PIR_VEC_FREE(&new_insts);
                        return false;
                    }
                    uint32_t al = (copy.align > ly.align) ? copy.align : ly.align;
                    PirInst *slot = &fn->insts.data[iid];
                    slot->op       = PIR_OP_ALLOCA_PTR;
                    slot->ty       = pir_type_scalar(PIR_SCALAR_PTR);
                    slot->aux_type = pir_type_scalar(PIR_SCALAR_VOID);
                    slot->imm_u64  = ly.size > 0u ? ly.size : 1u;
                    slot->align    = al > 0u ? al : 1u;
                    PIR_VEC_PUSH(&new_insts, iid);
                } else if (copy.op == PIR_OP_FIELD_PTR) {
                    uint64_t off = pir_field_offset(mod, target, copy.aux_type.u.agg_id,
                                                    (uint32_t)copy.imm_u64);
                    if (off == 0u && copy.operands[0].kind == PIR_OPER_VAL) {
                        pir_fn_replace_val_uses(fn, copy.results[0], copy.operands[0].u.val);
                    } else {
                        PirInst *slot = &fn->insts.data[iid];
                        slot->op            = PIR_OP_PTR_ADD;
                        slot->ty            = ptr_int_ty;
                        slot->aux_type      = pir_type_scalar(PIR_SCALAR_VOID);
                        slot->flags         = PIR_FLAG_INBOUNDS;
                        slot->imm_u64       = 0u;
                        slot->operand_count = 2u;
                        slot->operands      = pir_alloc_ops(mod, 2u);
                        slot->operands[0]   = copy.operands[0];
                        slot->operands[1]   = pir_op_imm_i(off, false, ptr_int_ty);
                        PIR_VEC_PUSH(&new_insts, iid);
                    }
                } else if (copy.op == PIR_OP_ELEM_PTR) {
                    uint64_t stride = 0u;
                    if (copy.aux_type.kind == PIR_TYPE_NAMED_AGG &&
                        mod->agg_types.data[copy.aux_type.u.agg_id].kind == PIR_AGG_ARRAY) {
                        PirTypeLayout ly = pir_type_layout(mod, target, copy.aux_type);
                        stride = ly.stride;
                    } else {
                        PirTypeLayout ly = pir_type_layout(mod, target, copy.aux_type);
                        stride = align_up_u64(ly.size, ly.align);
                    }
                    PirOperand idx = copy.operands[1];
                    PirType idx_ty = (idx.type.kind == PIR_TYPE_SCALAR &&
                                      (idx.type.u.scalar == PIR_SCALAR_I32 ||
                                       idx.type.u.scalar == PIR_SCALAR_I64))
                                     ? idx.type : ptr_int_ty;

                    if (idx.kind == PIR_OPER_IMM_INT) {
                        uint64_t k = idx.u.imm_int.lo;
                        uint64_t off = k * stride;
                        if (off == 0u && copy.operands[0].kind == PIR_OPER_VAL) {
                            pir_fn_replace_val_uses(fn, copy.results[0], copy.operands[0].u.val);
                        } else {
                            PirInst *slot = &fn->insts.data[iid];
                            slot->op            = PIR_OP_PTR_ADD;
                            slot->ty            = idx_ty;
                            slot->aux_type      = pir_type_scalar(PIR_SCALAR_VOID);
                            slot->flags         = PIR_FLAG_INBOUNDS;
                            slot->operand_count = 2u;
                            slot->operands      = pir_alloc_ops(mod, 2u);
                            slot->operands[0]   = copy.operands[0];
                            slot->operands[1]   = pir_op_imm_i(off, idx.u.imm_int.is_negative, idx_ty);
                            PIR_VEC_PUSH(&new_insts, iid);
                        }
                    } else {
                        PirOperand byte_off = idx;
                        uint32_t sh = 0u;
                        if (stride == 1u) {
                            /* byte_off is already idx */
                        } else if (is_pow2_u64(stride, &sh)) {
                            PirInst s = pir_make_inst(PIR_OP_SHL, idx_ty, copy.src_line, copy.loc_idx);
                            s.operand_count = 2u;
                            s.operands      = pir_alloc_ops(mod, 2u);
                            s.operands[0]   = idx;
                            s.operands[1]   = pir_op_imm_i(sh, false, idx_ty);
                            s.results[0]    = pir_fn_new_val(mod, fn, "eoff", idx_ty, b, PIR_INVALID_ID, 0u);
                            pir_fn_append_inst(fn, &new_insts, s);
                            byte_off = pir_op_val(s.results[0], idx_ty);
                        } else {
                            PirInst m = pir_make_inst(PIR_OP_MUL, idx_ty, copy.src_line, copy.loc_idx);
                            m.operand_count = 2u;
                            m.operands      = pir_alloc_ops(mod, 2u);
                            m.operands[0]   = idx;
                            m.operands[1]   = pir_op_imm_i(stride, false, idx_ty);
                            m.results[0]    = pir_fn_new_val(mod, fn, "eoff", idx_ty, b, PIR_INVALID_ID, 0u);
                            pir_fn_append_inst(fn, &new_insts, m);
                            byte_off = pir_op_val(m.results[0], idx_ty);
                        }
                        PirInst *slot = &mod->funcs.data[fi].insts.data[iid];
                        slot->op            = PIR_OP_PTR_ADD;
                        slot->ty            = idx_ty;
                        slot->aux_type      = pir_type_scalar(PIR_SCALAR_VOID);
                        slot->flags         = PIR_FLAG_INBOUNDS;
                        slot->operand_count = 2u;
                        slot->operands      = pir_alloc_ops(mod, 2u);
                        slot->operands[0]   = copy.operands[0];
                        slot->operands[1]   = byte_off;
                        PIR_VEC_PUSH(&new_insts, iid);
                    }
                } else if (copy.op == PIR_OP_MEMCPY || copy.op == PIR_OP_MEMMOVE ||
                           copy.op == PIR_OP_MEMSET) {
                    lower_mem_intrinsic(mod, target, fi, b, &new_insts, &copy);
                } else {
                    PIR_VEC_PUSH(&new_insts, iid);
                }
            }
            PIR_VEC_FREE(&mod->funcs.data[fi].blocks.data[b].insts);
            mod->funcs.data[fi].blocks.data[b].insts = new_insts;
        }
    }

    /* 2. Flatten typed aggregate globals into byte-count zeroinit / raw data. */
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        PirGlobal *g = &mod->globals.data[i];
        if (g->has_type && g->type.kind == PIR_TYPE_NAMED_AGG) {
            PirType orig_ty = g->type;
            PirTypeLayout ly = pir_type_layout(mod, target, orig_ty);
            if (ly.overflow) {
                pir_diag_error(diag, g->src_line, 0u, "global aggregate type size overflow");
                return false;
            }
            if (g->align < ly.align) g->align = ly.align;
            g->has_type = false;
            g->type     = pir_type_scalar(PIR_SCALAR_VOID);
            if (g->is_zeroinit || g->item_count == 0u) {
                g->is_zeroinit = true;
                g->is_braced   = false;
                g->item_count  = 1u;
                g->items       = PIR_ARENA_NEW_ARRAY(&mod->arena, PirGlobalItem, 1u);
                g->items[0].kind = PIR_GITEM_ZERO;
                g->items[0].len  = ly.size > 0u ? ly.size : 1u;
            } else {
                GItemVec flat = {0};
                uint32_t idx = 0u;
                flatten_agg_global(mod, target, orig_ty, g->items, g->item_count, &idx, &flat);
                g->is_braced  = true;
                g->item_count = flat.len;
                if (flat.len > 0u) {
                    g->items = PIR_ARENA_NEW_ARRAY(&mod->arena, PirGlobalItem, flat.len);
                    memcpy(g->items, flat.data, flat.len * sizeof(PirGlobalItem));
                } else {
                    g->items = NULL;
                }
                PIR_VEC_FREE(&flat);
            }
        }
    }

    /* 3. Remove all HIR aggregate type declarations. */
    mod->agg_types.len = 0u;
    return true;
}
