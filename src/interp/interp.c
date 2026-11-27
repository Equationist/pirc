/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "interp/interp.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIR_MAX_CALL_DEPTH 1024u
#define PIR_MAX_ALLOC_SIZE (UINT64_C(1) << 30)

/* =========================================================================
 * Diagnostics & Provenance Memory Engine
 * ========================================================================= */

void pir_interp_fail(PirInterp *it, PirInterpStatus st, const char *fmt, ...) {
    if (it->status != PIR_INTERP_OK) return;
    it->status = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it->err_msg, sizeof(it->err_msg), fmt, ap);
    va_end(ap);
}

static uint64_t align_up_u64(uint64_t v, uint64_t align) {
    uint64_t m = align - 1u;
    return (v + m) & ~m;
}

uint32_t pir_interp_alloc_new(PirInterp *it, size_t size, uint32_t align,
                              bool readonly, bool is_heap, bool is_dyn, bool zero_init) {
    if ((uint64_t)size > PIR_MAX_ALLOC_SIZE) {
        if (!is_heap) {
            pir_interp_fail(it, PIR_INTERP_ERR, "allocation size %zu exceeds interpreter limit", size);
        }
        return 0u;
    }
    if (align == 0u) align = 1u;
    uint64_t a_align = align < 16u ? 16u : (uint64_t)align;
    if (it->next_addr > UINT64_MAX - a_align) {
        pir_interp_fail(it, PIR_INTERP_ERR, "interpreter virtual address space exhausted");
        return 0u;
    }
    uint64_t base = align_up_u64(it->next_addr, a_align);
    uint64_t span = (uint64_t)size + 4096u;
    if (span < (uint64_t)size || base > UINT64_MAX - span ||
        (it->ptr_bytes == 4u && base + span > UINT32_MAX)) {
        pir_interp_fail(it, PIR_INTERP_ERR, "interpreter virtual address space exhausted");
        return 0u;
    }
    it->next_addr = base + span; /* Guard gap prevents cross-object aliasing */

    PirAlloc al;
    memset(&al, 0, sizeof(al));
    size_t alloc_sz = size > 0u ? size : 1u;
    al.bytes        = (uint8_t *)calloc(alloc_sz, 1u);
    al.init_bits    = (uint8_t *)calloc(alloc_sz, 1u);
    if (!al.bytes || !al.init_bits) {
        free(al.bytes);
        free(al.init_bits);
        if (!is_heap) {
            pir_interp_fail(it, PIR_INTERP_ERR, "out of memory allocating %zu bytes", size);
        }
        return 0u;
    }
    if (zero_init && size > 0u) {
        memset(al.init_bits, 1, size);
    }
    al.size         = size;
    al.align        = align;
    al.base_addr    = base;
    al.alive        = true;
    al.readonly     = readonly;
    al.is_heap      = is_heap;
    al.is_dyn_stack = is_dyn;
    al.func_id      = PIR_INVALID_ID;

    uint32_t id = it->allocs.len;
    PIR_VEC_PUSH(&it->allocs, al);
    return id;
}

uint64_t pir_interp_ptr_to_int(const PirInterp *it, PirPtr p) {
    uint64_t pmask = (it->ptr_bytes >= 8u) ? UINT64_MAX : (((uint64_t)1u << (it->ptr_bytes * 8u)) - 1u);
    if ((p.alloc_id & 0x80000000u) != 0u) {
        /* Preserve frame_id + token_id for stacksave sentinel pointers. */
        uint64_t fid = (uint64_t)(p.alloc_id & 0x7FFFFFFFu);
        uint64_t tok = (uint64_t)p.offset & 0xFFFFFFFFu;
        return ((fid << 32u) | tok) & pmask;
    }
    if (p.alloc_id == 0u || p.alloc_id >= it->allocs.len) {
        return (uint64_t)p.offset & pmask;
    }
    return (it->allocs.data[p.alloc_id].base_addr + (uint64_t)p.offset) & pmask;
}

PirPtr pir_interp_int_to_ptr(PirInterp *it, uint64_t addr) {
    uint64_t pmask = (it->ptr_bytes >= 8u) ? UINT64_MAX : (((uint64_t)1u << (it->ptr_bytes * 8u)) - 1u);
    addr &= pmask;
    PirPtr res = { 0u, 0 };
    if (addr == 0u) return res;
    for (uint32_t i = 1u; i < it->allocs.len; ++i) {
        const PirAlloc *a = &it->allocs.data[i];
        uint64_t base = a->base_addr & pmask;
        if (addr >= base && addr <= base + (uint64_t)a->size) {
            res.alloc_id = i;
            res.offset   = (int64_t)(addr - base);
            return res;
        }
    }
    /* Fallback: recover provenance for wrapping OOB pointers within the 2048B half-guard window
     * (e.g. `ptr_add %arr, -8` round-tripped through byval/retbuf integer register packing). */
    for (uint32_t i = 1u; i < it->allocs.len; ++i) {
        const PirAlloc *a = &it->allocs.data[i];
        uint64_t base = a->base_addr & pmask;
        int64_t rel = (it->ptr_bytes == 4u)
                      ? (int64_t)(int32_t)((uint32_t)addr - (uint32_t)base)
                      : (int64_t)(addr - base);
        if (rel >= -2048 && rel <= (int64_t)a->size + 2048) {
            res.alloc_id = i;
            res.offset   = rel;
            return res;
        }
    }
    res.alloc_id = 0u;
    res.offset   = (int64_t)addr;
    return res;
}

bool pir_interp_check_access(PirInterp *it, PirPtr p, size_t size,
                             uint32_t align, bool is_write, bool check_init) {
    if (p.alloc_id == 0u) {
        pir_interp_fail(it, PIR_INTERP_UB, "null pointer dereference");
        return false;
    }
    if (p.alloc_id >= it->allocs.len) {
        pir_interp_fail(it, PIR_INTERP_UB, "invalid allocation id %u", p.alloc_id);
        return false;
    }
    const PirAlloc *a = &it->allocs.data[p.alloc_id];
    if (!a->alive) {
        pir_interp_fail(it, PIR_INTERP_UB, "use_after_free on allocation %u", p.alloc_id);
        return false;
    }
    if (a->is_func) {
        pir_interp_fail(it, PIR_INTERP_UB, "cannot load/store function pointer");
        return false;
    }
    if (is_write && a->readonly) {
        pir_interp_fail(it, PIR_INTERP_UB, "write to read-only memory");
        return false;
    }
    if (p.offset < 0 || (uint64_t)p.offset > (uint64_t)a->size ||
        (uint64_t)size > (uint64_t)a->size - (uint64_t)p.offset) {
        pir_interp_fail(it, PIR_INTERP_UB,
                        "out_of_bounds memory access (offset %" PRId64 ", size %zu, alloc size %zu)",
                        p.offset, size, a->size);
        return false;
    }
    if (align > 1u) {
        uint64_t addr = a->base_addr + (uint64_t)p.offset;
        if ((addr % (uint64_t)align) != 0u) {
            pir_interp_fail(it, PIR_INTERP_UB,
                            "alignment violation (offset %" PRId64 ", required align %u)",
                            p.offset, align);
            return false;
        }
    }
    if (check_init) {
        size_t off = (size_t)p.offset;
        for (size_t i = 0u; i < size; ++i) {
            if (a->init_bits[off + i] == 0u) {
                pir_interp_fail(it, PIR_INTERP_UB,
                                "uninitialized memory read at offset %zu", off + i);
                return false;
            }
        }
    }
    return true;
}

void pir_interp_clear_ptr_tags(PirAlloc *a, size_t off, size_t size, uint8_t ptr_bytes) {
    for (uint32_t i = 0u; i < a->ptr_tags.len; ) {
        size_t to = a->ptr_tags.data[i].byte_off;
        if (to + ptr_bytes > off && to < off + size) {
            a->ptr_tags.data[i] = a->ptr_tags.data[--a->ptr_tags.len];
        } else {
            i++;
        }
    }
}

bool pir_ptr_offset_checked(PirInterp *it, PirPtr base, int64_t delta, PirPtr *out) {
    if ((delta > 0 && base.offset > INT64_MAX - delta) ||
        (delta < 0 && base.offset < INT64_MIN - delta)) {
        pir_interp_fail(it, PIR_INTERP_UB, "pointer offset overflow");
        return false;
    }
    out->alloc_id = base.alloc_id;
    out->offset   = base.offset + delta;
    return true;
}

bool pir_interp_mem_read(PirInterp *it, PirPtr p, void *dst, size_t size, uint32_t align) {
    if (!pir_interp_check_access(it, p, size, align, false, true)) return false;
    memcpy(dst, it->allocs.data[p.alloc_id].bytes + p.offset, size);
    return true;
}

bool pir_interp_mem_write(PirInterp *it, PirPtr p, const void *src, size_t size, uint32_t align) {
    if (!pir_interp_check_access(it, p, size, align, true, false)) return false;
    PirAlloc *a = &it->allocs.data[p.alloc_id];
    size_t off = (size_t)p.offset;
    memcpy(a->bytes + off, src, size);
    memset(a->init_bits + off, 1, size);
    pir_interp_clear_ptr_tags(a, off, size, it->ptr_bytes);
    return true;
}

bool pir_interp_mem_write_ptr(PirInterp *it, PirPtr p, PirPtr val, uint32_t align) {
    uint8_t pb = it->ptr_bytes;
    uint64_t addr = pir_interp_ptr_to_int(it, val);
    if (!pir_interp_mem_write(it, p, &addr, pb, align)) return false;
    PirAlloc *a = &it->allocs.data[p.alloc_id];
    PirPtrTag tag = { (size_t)p.offset, val };
    PIR_VEC_PUSH(&a->ptr_tags, tag);
    return true;
}

bool pir_interp_mem_read_ptr(PirInterp *it, PirPtr p, PirPtr *out, uint32_t align) {
    uint8_t pb = it->ptr_bytes;
    uint64_t addr = 0u;
    if (!pir_interp_mem_read(it, p, &addr, pb, align)) return false;
    const PirAlloc *a = &it->allocs.data[p.alloc_id];
    size_t off = (size_t)p.offset;
    for (uint32_t i = 0u; i < a->ptr_tags.len; ++i) {
        if (a->ptr_tags.data[i].byte_off == off) {
            *out = a->ptr_tags.data[i].ptr;
            return true;
        }
    }
    *out = pir_interp_int_to_ptr(it, addr);
    return true;
}

bool pir_interp_mem_copy(PirInterp *it, PirPtr dst, PirPtr src, size_t len, bool overlap_ok) {
    if (len == 0u) return true;
    if (!pir_interp_check_access(it, src, len, 1u, false, false)) return false;
    if (!pir_interp_check_access(it, dst, len, 1u, true, false)) return false;
    if (!overlap_ok && dst.alloc_id == src.alloc_id) {
        size_t d0 = (size_t)dst.offset, s0 = (size_t)src.offset;
        if (d0 < s0 + len && s0 < d0 + len) {
            pir_interp_fail(it, PIR_INTERP_UB, "overlapping memcpy");
            return false;
        }
    }
    PirAlloc *da = &it->allocs.data[dst.alloc_id];
    const PirAlloc *sa = &it->allocs.data[src.alloc_id];
    size_t doff = (size_t)dst.offset;
    size_t soff = (size_t)src.offset;

    /* Snapshot complete ptr tags in source range before mutating destination. */
    PIR_VEC(PirPtrTag) moved = {0};
    for (uint32_t i = 0u; i < sa->ptr_tags.len; ++i) {
        size_t to = sa->ptr_tags.data[i].byte_off;
        if (to >= soff && to + it->ptr_bytes <= soff + len) {
            PirPtrTag nt = { doff + (to - soff), sa->ptr_tags.data[i].ptr };
            PIR_VEC_PUSH(&moved, nt);
        }
    }
    memmove(da->bytes + doff, sa->bytes + soff, len);
    memmove(da->init_bits + doff, sa->init_bits + soff, len);
    pir_interp_clear_ptr_tags(da, doff, len, it->ptr_bytes);
    for (uint32_t i = 0u; i < moved.len; ++i) {
        PIR_VEC_PUSH(&da->ptr_tags, moved.data[i]);
    }
    PIR_VEC_FREE(&moved);
    return true;
}

/* =========================================================================
 * Scalar & Lane Helpers (Zero Host-C UB)
 * ========================================================================= */

static size_t type_byte_size(PirType ty, uint8_t ptr_bytes) {
    return (size_t)(pir_type_bits(ty, ptr_bytes) / 8u);
}

static uint64_t width_mask(unsigned bits) {
    return (bits >= 64u) ? UINT64_MAX : (((uint64_t)1u << bits) - 1u);
}

static int64_t sext_u64(uint64_t x, unsigned bits) {
    x &= width_mask(bits);
    if (bits < 64u && (x & ((uint64_t)1u << (bits - 1u))) != 0u) {
        x |= ~width_mask(bits);
    }
    return (int64_t)x;
}

static int64_t smin_bits(unsigned bits) {
    return (bits == 64u) ? INT64_MIN : -(int64_t)((uint64_t)1u << (bits - 1u));
}

static int64_t smax_bits(unsigned bits) {
    return (bits == 64u) ? INT64_MAX : (int64_t)(((uint64_t)1u << (bits - 1u)) - 1u);
}

uint64_t pir_val_get_u64(const PirVal *v) {
    uint64_t x = 0u;
    if (v->ty.kind == PIR_TYPE_SCALAR) {
        switch (v->ty.u.scalar) {
            case PIR_SCALAR_I8:  { uint8_t  b = 0u; memcpy(&b, v->bytes, 1u); return b; }
            case PIR_SCALAR_I16: { uint16_t b = 0u; memcpy(&b, v->bytes, 2u); return b; }
            case PIR_SCALAR_I32: { uint32_t b = 0u; memcpy(&b, v->bytes, 4u); return b; }
            case PIR_SCALAR_I64: { memcpy(&x, v->bytes, 8u); return x; }
            case PIR_SCALAR_F32: { uint32_t b = 0u; memcpy(&b, v->bytes, 4u); return b; }
            case PIR_SCALAR_F64: { memcpy(&x, v->bytes, 8u); return x; }
            case PIR_SCALAR_VOID: case PIR_SCALAR_I128:
            case PIR_SCALAR_PTR:  case PIR_SCALAR_VALIST:
                return 0u;
        }
    }
    return 0u;
}

void pir_val_set_u64(PirVal *v, PirType ty, uint64_t x) {
    memset(v, 0, sizeof(*v));
    v->ty = ty;
    if (ty.kind == PIR_TYPE_SCALAR) {
        switch (ty.u.scalar) {
            case PIR_SCALAR_I8:  { uint8_t  b = (uint8_t)x;  memcpy(v->bytes, &b, 1u); break; }
            case PIR_SCALAR_I16: { uint16_t b = (uint16_t)x; memcpy(v->bytes, &b, 2u); break; }
            case PIR_SCALAR_I32: case PIR_SCALAR_F32:
                                 { uint32_t b = (uint32_t)x; memcpy(v->bytes, &b, 4u); break; }
            case PIR_SCALAR_I64: case PIR_SCALAR_F64:
                                 { memcpy(v->bytes, &x, 8u); break; }
            case PIR_SCALAR_VOID: case PIR_SCALAR_I128:
            case PIR_SCALAR_PTR:  case PIR_SCALAR_VALIST:
                break;
        }
    }
}

static uint64_t read_lane_u64(const PirVal *v, uint32_t lane, unsigned elem_bits) {
    size_t sz = elem_bits / 8u;
    uint64_t x = 0u;
    memcpy(&x, v->bytes + lane * sz, sz);
    return x;
}

static void write_lane_u64(PirVal *v, uint32_t lane, unsigned elem_bits, uint64_t x) {
    size_t sz = elem_bits / 8u;
    x &= width_mask(elem_bits);
    memcpy(v->bytes + lane * sz, &x, sz);
}

static double read_lane_f64(const PirVal *v, uint32_t lane, PirScalarType sc) {
    if (sc == PIR_SCALAR_F32) {
        float f = 0.0f;
        memcpy(&f, v->bytes + lane * 4u, 4u);
        return (double)f;
    }
    double d = 0.0;
    memcpy(&d, v->bytes + lane * 8u, 8u);
    return d;
}

static void write_lane_f64(PirVal *v, uint32_t lane, PirScalarType sc, double d) {
    if (sc == PIR_SCALAR_F32) {
        float f = (float)d;
        memcpy(v->bytes + lane * 4u, &f, 4u);
    } else {
        memcpy(v->bytes + lane * 8u, &d, 8u);
    }
}

/* Portable 64x64 -> 128-bit unsigned multiply using 32-bit partial products. */
static void mul_u64_wide(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32u;
    uint64_t b0 = (uint32_t)b, b1 = b >> 32u;
    uint64_t p00 = a0 * b0;
    uint64_t p01 = a0 * b1;
    uint64_t p10 = a1 * b0;
    uint64_t p11 = a1 * b1;
    uint64_t mid = (p00 >> 32u) + (uint32_t)p01 + (uint32_t)p10;
    *lo = (p00 & 0xFFFFFFFFu) | (mid << 32u);
    *hi = p11 + (p01 >> 32u) + (p10 >> 32u) + (mid >> 32u);
}

static void mul_s64_wide(int64_t sa, int64_t sb, uint64_t *lo, uint64_t *hi) {
    uint64_t ua = (uint64_t)sa, ub = (uint64_t)sb;
    mul_u64_wide(ua, ub, lo, hi);
    if (sa < 0) *hi -= ub;
    if (sb < 0) *hi -= ua;
}

static bool eval_int_binop_lane(PirInterp *it, PirOpcode op, uint32_t flags,
                                unsigned bits, uint64_t ua, uint64_t ub, uint64_t *out) {
    uint64_t mask = width_mask(bits);
    ua &= mask;
    ub &= mask;
    int64_t sa = sext_u64(ua, bits);
    int64_t sb = sext_u64(ub, bits);

    if (op == PIR_OP_ADD) {
        uint64_t res = (ua + ub) & mask;
        bool ovf_u = (bits == 64u) ? (res < ua) : ((ua + ub) > mask);
        int64_t sr = sext_u64(res, bits);
        bool ovf_s = ((sa > 0 && sb > 0 && sr < 0) || (sa < 0 && sb < 0 && sr >= 0));
        if ((flags & PIR_FLAG_TRAP_S) && ovf_s)   { pir_interp_fail(it, PIR_INTERP_TRAP, "signed_overflow in add"); return false; }
        if ((flags & PIR_FLAG_TRAP_U) && ovf_u)   { pir_interp_fail(it, PIR_INTERP_TRAP, "unsigned_overflow in add"); return false; }
        if ((flags & PIR_FLAG_UNSAFE_S) && ovf_s) { pir_interp_fail(it, PIR_INTERP_UB, "signed_overflow in add [unsafe_s]"); return false; }
        if ((flags & PIR_FLAG_UNSAFE_U) && ovf_u) { pir_interp_fail(it, PIR_INTERP_UB, "unsigned_overflow in add [unsafe_u]"); return false; }
        *out = res;
        return true;
    }
    if (op == PIR_OP_SUB) {
        uint64_t res = (ua - ub) & mask;
        bool ovf_u = (ua < ub);
        int64_t sr = sext_u64(res, bits);
        bool ovf_s = ((sa >= 0 && sb < 0 && sr < 0) || (sa < 0 && sb > 0 && sr >= 0));
        if ((flags & PIR_FLAG_TRAP_S) && ovf_s)   { pir_interp_fail(it, PIR_INTERP_TRAP, "signed_overflow in sub"); return false; }
        if ((flags & PIR_FLAG_TRAP_U) && ovf_u)   { pir_interp_fail(it, PIR_INTERP_TRAP, "unsigned_overflow in sub"); return false; }
        if ((flags & PIR_FLAG_UNSAFE_S) && ovf_s) { pir_interp_fail(it, PIR_INTERP_UB, "signed_overflow in sub [unsafe_s]"); return false; }
        if ((flags & PIR_FLAG_UNSAFE_U) && ovf_u) { pir_interp_fail(it, PIR_INTERP_UB, "unsigned_overflow in sub [unsafe_u]"); return false; }
        *out = res;
        return true;
    }
    if (op == PIR_OP_MUL) {
        uint64_t lo = 0u, hi = 0u;
        mul_u64_wide(ua, ub, &lo, &hi);
        uint64_t res = lo & mask;
        bool ovf_u = (bits == 64u) ? (hi != 0u) : (lo > mask);
        uint64_t slo = 0u, shi = 0u;
        mul_s64_wide(sa, sb, &slo, &shi);
        int64_t sr = sext_u64(slo, bits);
        bool ovf_s = (bits == 64u) ? ((int64_t)shi != (sr < 0 ? -1 : 0))
                                   : ((int64_t)slo < smin_bits(bits) || (int64_t)slo > smax_bits(bits));
        if ((flags & PIR_FLAG_TRAP_S) && ovf_s)   { pir_interp_fail(it, PIR_INTERP_TRAP, "signed_overflow in mul"); return false; }
        if ((flags & PIR_FLAG_TRAP_U) && ovf_u)   { pir_interp_fail(it, PIR_INTERP_TRAP, "unsigned_overflow in mul"); return false; }
        if ((flags & PIR_FLAG_UNSAFE_S) && ovf_s) { pir_interp_fail(it, PIR_INTERP_UB, "signed_overflow in mul [unsafe_s]"); return false; }
        if ((flags & PIR_FLAG_UNSAFE_U) && ovf_u) { pir_interp_fail(it, PIR_INTERP_UB, "unsigned_overflow in mul [unsafe_u]"); return false; }
        *out = res;
        return true;
    }
    if (op == PIR_OP_DIV_S || op == PIR_OP_REM_S) {
        if (sb == 0) {
            pir_interp_fail(it, (flags & PIR_FLAG_UNSAFE) ? PIR_INTERP_UB : PIR_INTERP_TRAP,
                            "signed division by zero");
            return false;
        }
        if (sa == smin_bits(bits) && sb == -1) {
            if (flags & PIR_FLAG_WRAP) {
                *out = (op == PIR_OP_DIV_S) ? ((uint64_t)smin_bits(bits) & mask) : 0u;
                return true;
            }
            pir_interp_fail(it, (flags & PIR_FLAG_UNSAFE) ? PIR_INTERP_UB : PIR_INTERP_TRAP,
                            "signed division overflow (INT_MIN / -1)");
            return false;
        }
        *out = (uint64_t)(op == PIR_OP_DIV_S ? (sa / sb) : (sa % sb)) & mask;
        return true;
    }
    if (op == PIR_OP_DIV_U || op == PIR_OP_REM_U) {
        if (ub == 0u) {
            pir_interp_fail(it, (flags & PIR_FLAG_UNSAFE) ? PIR_INTERP_UB : PIR_INTERP_TRAP,
                            "unsigned division by zero");
            return false;
        }
        *out = (op == PIR_OP_DIV_U ? (ua / ub) : (ua % ub)) & mask;
        return true;
    }
    if (op == PIR_OP_AND) { *out = (ua & ub) & mask; return true; }
    if (op == PIR_OP_OR)  { *out = (ua | ub) & mask; return true; }
    if (op == PIR_OP_XOR) { *out = (ua ^ ub) & mask; return true; }
    if (op == PIR_OP_SHL || op == PIR_OP_SHR || op == PIR_OP_SAR) {
        if (ub >= bits) {
            if (flags & PIR_FLAG_TRAP) {
                pir_interp_fail(it, PIR_INTERP_TRAP, "shift amount %" PRIu64 " >= %u", ub, bits);
                return false;
            }
            if (flags & PIR_FLAG_UNSAFE) {
                pir_interp_fail(it, PIR_INTERP_UB, "shift amount %" PRIu64 " >= %u [unsafe]", ub, bits);
                return false;
            }
            ub &= (uint64_t)(bits - 1u);
        }
        unsigned sh = (unsigned)ub;
        if (op == PIR_OP_SHL) {
            *out = (ua << sh) & mask;
        } else if (op == PIR_OP_SHR) {
            *out = (ua >> sh) & mask;
        } else {
            uint64_t shifted = ua >> sh;
            if (sh > 0u && (ua & ((uint64_t)1u << (bits - 1u))) != 0u) {
                shifted |= (mask << (bits - sh)) & mask;
            }
            *out = shifted & mask;
        }
        return true;
    }
    return false;
}

static bool eval_fpto_int_lane(PirInterp *it, PirOpcode op, uint32_t flags,
                               unsigned dst_bits, double d, uint64_t *out) {
    bool is_signed = (op == PIR_OP_FPTOSI);
    double lo_lim = is_signed ? -(double)((uint64_t)1u << (dst_bits - 1u)) : 0.0;
    double hi_lim = is_signed ?  (double)((uint64_t)1u << (dst_bits - 1u))
                              : (dst_bits == 64u ? 18446744073709551616.0
                                                 : (double)((uint64_t)1u << dst_bits));
    double td = trunc(d);
    bool bad = isnan(d) || isinf(d) || td < lo_lim || td >= hi_lim;
    if (bad) {
        if (flags & PIR_FLAG_TRAP) {
            pir_interp_fail(it, PIR_INTERP_TRAP, "float-to-int out of range or NaN");
            return false;
        }
        if (flags & PIR_FLAG_UNSAFE) {
            pir_interp_fail(it, PIR_INTERP_UB, "float-to-int out of range or NaN [unsafe]");
            return false;
        }
        /* Default / [sat] */
        if (isnan(d))       { *out = 0u; return true; }
        if (d < lo_lim)     { *out = is_signed ? ((uint64_t)smin_bits(dst_bits) & width_mask(dst_bits)) : 0u; return true; }
        *out = is_signed ? (uint64_t)smax_bits(dst_bits) : width_mask(dst_bits);
        return true;
    }
    if (is_signed) {
        *out = (uint64_t)(int64_t)d & width_mask(dst_bits);
    } else {
        *out = (uint64_t)d & width_mask(dst_bits);
    }
    return true;
}

static bool eval_fcmp_lane(PirOpcode op, double a, double b) {
    bool un = isnan(a) || isnan(b);
    if (op == PIR_OP_FCMP_OEQ) return !un && a == b;
    if (op == PIR_OP_FCMP_ONE) return !un && a != b;
    if (op == PIR_OP_FCMP_OLT) return !un && a <  b;
    if (op == PIR_OP_FCMP_OLE) return !un && a <= b;
    if (op == PIR_OP_FCMP_OGT) return !un && a >  b;
    if (op == PIR_OP_FCMP_OGE) return !un && a >= b;
    if (op == PIR_OP_FCMP_UEQ) return  un || a == b;
    if (op == PIR_OP_FCMP_UNE) return  un || a != b;
    if (op == PIR_OP_FCMP_ULT) return  un || a <  b;
    if (op == PIR_OP_FCMP_ULE) return  un || a <= b;
    if (op == PIR_OP_FCMP_UGT) return  un || a >  b;
    if (op == PIR_OP_FCMP_UGE) return  un || a >= b;
    return false;
}

/* =========================================================================
 * Operand Evaluation & Function Execution
 * ========================================================================= */

typedef struct {
    uint32_t token_id;
    uint32_t dyn_mark;
} StackSaveMark;

typedef struct {
    const PirFunction      *fn;
    uint32_t                frame_id;
    PirVal                 *vals;
    uint32_t               *static_allocas;
    PIR_VEC(uint32_t)       stack_allocs;
    PIR_VEC(uint32_t)       dyn_stack_allocs;
    PIR_VEC(StackSaveMark)  active_saves;
    uint32_t                next_save_id;
    const PirVal           *varargs;
    const PirCallArgMeta   *vararg_meta;
    uint32_t                vararg_count;
    bool                    has_setjmp;
    PirPtr                  setjmp_jb;
    PirBlockId              setjmp_blk;
    uint32_t                setjmp_j;
    PirValueId              setjmp_res;
} CallFrame;

static PirVal eval_operand(PirInterp *it, const CallFrame *fr, const PirOperand *op) {
    PirVal v;
    memset(&v, 0, sizeof(v));
    v.ty = op->type;
    switch (op->kind) {
        case PIR_OPER_VAL:
            return fr->vals[op->u.val];
        case PIR_OPER_IMM_INT: {
            uint64_t x = op->u.imm_int.is_negative ? (UINT64_C(0) - op->u.imm_int.lo)
                                                   : op->u.imm_int.lo;
            if (op->type.kind == PIR_TYPE_SCALAR && op->type.u.scalar == PIR_SCALAR_PTR) {
                v.ptr = pir_interp_int_to_ptr(it, x);
            } else {
                pir_val_set_u64(&v, op->type, x);
            }
            return v;
        }
        case PIR_OPER_IMM_FLOAT:
            pir_val_set_u64(&v, op->type, op->u.imm_float.bits);
            return v;
        case PIR_OPER_SYM: {
            const PirModule *mod = it->mod;
            int64_t sym_off = (it->ptr_bytes == 4u)
                              ? (int64_t)(int32_t)(uint32_t)op->u.sym.offset
                              : op->u.sym.offset;
            for (uint32_t i = 0u; i < mod->globals.len; ++i) {
                if (strcmp(mod->globals.data[i].name, op->u.sym.name) == 0) {
                    v.ptr.alloc_id = it->global_allocs[i];
                    v.ptr.offset   = sym_off;
                    return v;
                }
            }
            for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
                if (strcmp(mod->funcs.data[i].name, op->u.sym.name) == 0) {
                    v.ptr.alloc_id = it->func_allocs[i];
                    v.ptr.offset   = sym_off;
                    return v;
                }
            }
            pir_interp_fail(it, PIR_INTERP_ERR, "unresolved symbol '@%s'", op->u.sym.name);
            return v;
        }
        case PIR_OPER_INVOKE_RET:
        case PIR_OPER_INVOKE_EXN:
            pir_interp_fail(it, PIR_INTERP_ERR, "HIR invoke placeholder in LIR interpreter");
            return v;
    }
    return v;
}

static bool check_bool_i8(PirInterp *it, const PirVal *v, uint8_t *out) {
    uint64_t x = pir_val_get_u64(v);
    if (x != 0u && x != 1u) {
        pir_interp_fail(it, PIR_INTERP_UB, "invalid boolean i8 value %" PRIu64 " (must be 0 or 1)", x);
        return false;
    }
    *out = (uint8_t)x;
    return true;
}

static bool exec_function(PirInterp *it, PirFuncId func_id,
                          const PirVal *args, const PirCallArgMeta *arg_meta,
                          uint32_t arg_count, PirVal *out_ret);

static bool exec_call_target(PirInterp *it, PirFuncId cid,
                             const PirVal *args, const PirCallArgMeta *arg_meta,
                             uint32_t arg_count, PirVal *out_ret) {
    const PirFunction *callee = &it->mod->funcs.data[cid];
    if (callee->is_defined) {
        return exec_function(it, cid, args, arg_meta, arg_count, out_ret);
    }
    if (pir_interp_try_shim(it, callee->name, args, arg_count, callee->ret_type, out_ret)) {
        return it->status == PIR_INTERP_OK;
    }
    pir_interp_fail(it, PIR_INTERP_ERR, "unimplemented extern function '@%s'", callee->name);
    return false;
}

static void transfer_branch(PirInterp *it, CallFrame *fr, const PirBranchTarget *bt,
                            PirBlockId *cur_blk) {
    const PirBlock *dst = &fr->fn->blocks.data[bt->target];
    PirVal *tmp = NULL;
    if (bt->arg_count > 0u) {
        tmp = (PirVal *)malloc(bt->arg_count * sizeof(PirVal));
        if (!tmp) abort();
        for (uint32_t k = 0u; k < bt->arg_count; ++k) {
            tmp[k] = eval_operand(it, fr, &bt->args[k]);
        }
        for (uint32_t k = 0u; k < bt->arg_count; ++k) {
            fr->vals[dst->params.data[k]] = tmp[k];
        }
        free(tmp);
    }
    *cur_blk = bt->target;
}

static bool exec_function(PirInterp *it, PirFuncId func_id,
                          const PirVal *args, const PirCallArgMeta *arg_meta,
                          uint32_t arg_count, PirVal *out_ret) {
    if (it->call_depth >= PIR_MAX_CALL_DEPTH) {
        pir_interp_fail(it, PIR_INTERP_UB, "interpreter stack overflow (depth > %u)", PIR_MAX_CALL_DEPTH);
        return false;
    }
    const PirFunction *fn = &it->mod->funcs.data[func_id];
    CallFrame fr;
    memset(&fr, 0, sizeof(fr));
    fr.fn = fn;
    fr.frame_id = ++it->next_frame_id;
    fr.vals = (PirVal *)calloc(fn->values.len + 1u, sizeof(PirVal));
    fr.static_allocas = (uint32_t *)calloc(fn->insts.len + 1u, sizeof(uint32_t));
    if (!fr.vals || !fr.static_allocas) abort();
    for (uint32_t i = 0u; i < fn->param_count; ++i) {
        if (fn->params[i].val_id < fn->values.len) {
            if (i < arg_count) {
                fr.vals[fn->params[i].val_id] = args[i];
            } else {
                fr.vals[fn->params[i].val_id].ty = fn->params[i].type;
            }
        }
    }
    if (fn->is_variadic && arg_count > fn->param_count) {
        fr.varargs      = args + fn->param_count;
        fr.vararg_meta  = arg_meta ? (arg_meta + fn->param_count) : NULL;
        fr.vararg_count = arg_count - fn->param_count;
    }

    it->call_depth++;
    PirBlockId cur_blk = 0u;
    uint32_t   start_j = 0u;
    bool returned = false;

    while (!returned && it->status == PIR_INTERP_OK && !it->exited && !it->unwinding) {
        const PirBlock *blk = &fn->blocks.data[cur_blk];
        uint32_t j = start_j;
        start_j = 0u;
        for (; j < blk->insts.len && !returned && it->status == PIR_INTERP_OK && !it->exited; ++j) {
            const PirInst *inst = &fn->insts.data[blk->insts.data[j]];
            PirVal r0, r1;
            memset(&r0, 0, sizeof(r0));
            memset(&r1, 0, sizeof(r1));
            if (inst->results[0] != PIR_INVALID_ID) r0.ty = fn->values.data[inst->results[0]].type;
            if (inst->results[1] != PIR_INVALID_ID) r1.ty = fn->values.data[inst->results[1]].type;

            switch (inst->op) {
                case PIR_OP_CONST:
                case PIR_OP_ADDR:
                    r0 = eval_operand(it, &fr, &inst->operands[0]);
                    r0.ty = inst->ty;
                    break;

                case PIR_OP_ADD: case PIR_OP_SUB: case PIR_OP_MUL:
                case PIR_OP_DIV_S: case PIR_OP_DIV_U: case PIR_OP_REM_S: case PIR_OP_REM_U:
                case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR:
                case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR: {
                    PirVal a = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal b = eval_operand(it, &fr, &inst->operands[1]);
                    uint32_t lanes = (inst->ty.kind == PIR_TYPE_VECTOR) ? pir_vec_lanes(inst->ty.u.vec) : 1u;
                    PirScalarType sc = (inst->ty.kind == PIR_TYPE_VECTOR)
                                       ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar;
                    unsigned bits = pir_scalar_bits(sc, it->ptr_bytes);
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        uint64_t ua = read_lane_u64(&a, l, bits);
                        uint64_t ub = read_lane_u64(&b, l, bits);
                        uint64_t out_l = 0u;
                        if (!eval_int_binop_lane(it, inst->op, inst->flags, bits, ua, ub, &out_l)) break;
                        write_lane_u64(&r0, l, bits, out_l);
                    }
                    break;
                }

                case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
                case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
                case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U: {
                    PirVal a = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal b = eval_operand(it, &fr, &inst->operands[1]);
                    unsigned bits = pir_scalar_bits(inst->ty.u.scalar, it->ptr_bytes);
                    uint64_t mask = width_mask(bits);
                    uint64_t ua = pir_val_get_u64(&a) & mask;
                    uint64_t ub = pir_val_get_u64(&b) & mask;
                    int64_t sa = sext_u64(ua, bits), sb = sext_u64(ub, bits);
                    uint64_t res = 0u;
                    bool ovf = false;
                    if (inst->op == PIR_OP_ADD_OVF_S || inst->op == PIR_OP_ADD_OVF_U) {
                        res = (ua + ub) & mask;
                        ovf = (inst->op == PIR_OP_ADD_OVF_U)
                              ? ((bits == 64u) ? (res < ua) : ((ua + ub) > mask))
                              : ((sa > 0 && sb > 0 && sext_u64(res, bits) < 0) ||
                                 (sa < 0 && sb < 0 && sext_u64(res, bits) >= 0));
                    } else if (inst->op == PIR_OP_SUB_OVF_S || inst->op == PIR_OP_SUB_OVF_U) {
                        res = (ua - ub) & mask;
                        ovf = (inst->op == PIR_OP_SUB_OVF_U)
                              ? (ua < ub)
                              : ((sa >= 0 && sb < 0 && sext_u64(res, bits) < 0) ||
                                 (sa < 0 && sb > 0 && sext_u64(res, bits) >= 0));
                    } else {
                        uint64_t lo = 0u, hi = 0u;
                        if (inst->op == PIR_OP_MUL_OVF_U) {
                            mul_u64_wide(ua, ub, &lo, &hi);
                            res = lo & mask;
                            ovf = (bits == 64u) ? (hi != 0u) : (lo > mask);
                        } else {
                            mul_s64_wide(sa, sb, &lo, &hi);
                            res = lo & mask;
                            int64_t sr = sext_u64(lo, bits);
                            ovf = (bits == 64u) ? ((int64_t)hi != (sr < 0 ? -1 : 0))
                                                : ((int64_t)lo < smin_bits(bits) || (int64_t)lo > smax_bits(bits));
                        }
                    }
                    pir_val_set_u64(&r0, inst->ty, res);
                    pir_val_set_u64(&r1, pir_type_scalar(PIR_SCALAR_I8), ovf ? 1u : 0u);
                    break;
                }

                case PIR_OP_ADD_CARRY:
                case PIR_OP_SUB_BORROW: {
                    PirVal a = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal b = eval_operand(it, &fr, &inst->operands[1]);
                    PirVal c = eval_operand(it, &fr, &inst->operands[2]);
                    unsigned bits = pir_scalar_bits(inst->ty.u.scalar, it->ptr_bytes);
                    uint64_t mask = width_mask(bits);
                    uint64_t ua = pir_val_get_u64(&a) & mask;
                    uint64_t ub = pir_val_get_u64(&b) & mask;
                    uint64_t ci = pir_val_get_u64(&c) & 1u;
                    uint64_t res = 0u, cout = 0u;
                    if (inst->op == PIR_OP_ADD_CARRY) {
                        if (bits == 64u) {
                            uint64_t s1 = ua + ub;
                            uint64_t c1 = (s1 < ua) ? 1u : 0u;
                            uint64_t s2 = s1 + ci;
                            uint64_t c2 = (s2 < s1) ? 1u : 0u;
                            res = s2; cout = c1 | c2;
                        } else {
                            uint64_t sum = ua + ub + ci;
                            res = sum & mask;
                            cout = (sum > mask) ? 1u : 0u;
                        }
                    } else {
                        if (bits == 64u) {
                            uint64_t b1 = (ua < ub) ? 1u : 0u;
                            uint64_t d1 = ua - ub;
                            uint64_t b2 = (d1 < ci) ? 1u : 0u;
                            res = d1 - ci; cout = b1 | b2;
                        } else {
                            res = (ua - ub - ci) & mask;
                            cout = (ua < ub + ci) ? 1u : 0u;
                        }
                    }
                    pir_val_set_u64(&r0, inst->ty, res);
                    pir_val_set_u64(&r1, pir_type_scalar(PIR_SCALAR_I8), cout);
                    break;
                }

                case PIR_OP_MUL_WIDE_U:
                case PIR_OP_MUL_WIDE_S: {
                    PirVal a = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal b = eval_operand(it, &fr, &inst->operands[1]);
                    uint64_t lo = 0u, hi = 0u;
                    if (inst->ty.u.scalar == PIR_SCALAR_I32) {
                        if (inst->op == PIR_OP_MUL_WIDE_U) {
                            uint64_t p = (uint64_t)(uint32_t)pir_val_get_u64(&a) *
                                         (uint64_t)(uint32_t)pir_val_get_u64(&b);
                            lo = (uint32_t)p; hi = (uint32_t)(p >> 32u);
                        } else {
                            int64_t p = (int64_t)(int32_t)pir_val_get_u64(&a) *
                                        (int64_t)(int32_t)pir_val_get_u64(&b);
                            lo = (uint32_t)(uint64_t)p; hi = (uint32_t)((uint64_t)p >> 32u);
                        }
                    } else {
                        if (inst->op == PIR_OP_MUL_WIDE_U) {
                            mul_u64_wide(pir_val_get_u64(&a), pir_val_get_u64(&b), &lo, &hi);
                        } else {
                            mul_s64_wide((int64_t)pir_val_get_u64(&a), (int64_t)pir_val_get_u64(&b), &lo, &hi);
                        }
                    }
                    pir_val_set_u64(&r0, inst->ty, lo);
                    pir_val_set_u64(&r1, inst->ty, hi);
                    break;
                }

                case PIR_OP_CLZ: case PIR_OP_CTZ: case PIR_OP_POPCNT: case PIR_OP_BSWAP: {
                    PirVal a = eval_operand(it, &fr, &inst->operands[0]);
                    unsigned bits = pir_scalar_bits(inst->ty.u.scalar, it->ptr_bytes);
                    uint64_t ua = pir_val_get_u64(&a) & width_mask(bits);
                    uint64_t res = 0u;
                    if (inst->op == PIR_OP_CLZ || inst->op == PIR_OP_CTZ) {
                        if (ua == 0u) {
                            if (inst->flags & PIR_FLAG_NONZERO) {
                                pir_interp_fail(it, PIR_INTERP_UB, "clz/ctz [nonzero] given 0");
                                break;
                            }
                            res = bits;
                        } else if (inst->op == PIR_OP_CLZ) {
                            for (unsigned k = 0u; k < bits; ++k) {
                                if ((ua & ((uint64_t)1u << (bits - 1u - k))) != 0u) break;
                                res++;
                            }
                        } else {
                            for (unsigned k = 0u; k < bits; ++k) {
                                if ((ua & ((uint64_t)1u << k)) != 0u) break;
                                res++;
                            }
                        }
                    } else if (inst->op == PIR_OP_POPCNT) {
                        for (uint64_t t = ua; t != 0u; t &= (t - 1u)) res++;
                    } else {
                        unsigned nbytes = bits / 8u;
                        for (unsigned k = 0u; k < nbytes; ++k) {
                            res |= ((ua >> (k * 8u)) & 0xFFu) << ((nbytes - 1u - k) * 8u);
                        }
                    }
                    pir_val_set_u64(&r0, inst->ty, res);
                    break;
                }

                case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV: {
                    PirVal a = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal b = eval_operand(it, &fr, &inst->operands[1]);
                    uint32_t lanes = (inst->ty.kind == PIR_TYPE_VECTOR) ? pir_vec_lanes(inst->ty.u.vec) : 1u;
                    PirScalarType sc = (inst->ty.kind == PIR_TYPE_VECTOR)
                                       ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar;
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        if (sc == PIR_SCALAR_F32) {
                            float fa = (float)read_lane_f64(&a, l, sc);
                            float fb = (float)read_lane_f64(&b, l, sc);
                            float fc = (inst->op == PIR_OP_FADD) ? (fa + fb)
                                     : (inst->op == PIR_OP_FSUB) ? (fa - fb)
                                     : (inst->op == PIR_OP_FMUL) ? (fa * fb) : (fa / fb);
                            write_lane_f64(&r0, l, sc, (double)fc);
                        } else {
                            double da = read_lane_f64(&a, l, sc);
                            double db = read_lane_f64(&b, l, sc);
                            double dc = (inst->op == PIR_OP_FADD) ? (da + db)
                                      : (inst->op == PIR_OP_FSUB) ? (da - db)
                                      : (inst->op == PIR_OP_FMUL) ? (da * db) : (da / db);
                            write_lane_f64(&r0, l, sc, dc);
                        }
                    }
                    break;
                }

                case PIR_OP_FNEG: {
                    PirVal a = eval_operand(it, &fr, &inst->operands[0]);
                    uint32_t lanes = (inst->ty.kind == PIR_TYPE_VECTOR) ? pir_vec_lanes(inst->ty.u.vec) : 1u;
                    PirScalarType sc = (inst->ty.kind == PIR_TYPE_VECTOR)
                                       ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar;
                    unsigned bits = pir_scalar_bits(sc, it->ptr_bytes);
                    uint64_t sign_bit = (bits == 32u) ? 0x80000000ULL : 0x8000000000000000ULL;
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        write_lane_u64(&r0, l, bits, read_lane_u64(&a, l, bits) ^ sign_bit);
                    }
                    break;
                }

                case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
                case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LT_U:
                case PIR_OP_CMP_LE_S: case PIR_OP_CMP_LE_U:
                case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GT_U:
                case PIR_OP_CMP_GE_S: case PIR_OP_CMP_GE_U: {
                    PirVal a = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal b = eval_operand(it, &fr, &inst->operands[1]);
                    if (inst->ty.kind == PIR_TYPE_SCALAR && inst->ty.u.scalar == PIR_SCALAR_PTR) {
                        uint64_t pa = pir_interp_ptr_to_int(it, a.ptr);
                        uint64_t pb = pir_interp_ptr_to_int(it, b.ptr);
                        bool c = (inst->op == PIR_OP_CMP_EQ)   ? (pa == pb)
                               : (inst->op == PIR_OP_CMP_NE)   ? (pa != pb)
                               : (inst->op == PIR_OP_CMP_LT_U) ? (pa <  pb)
                               : (inst->op == PIR_OP_CMP_LE_U) ? (pa <= pb)
                               : (inst->op == PIR_OP_CMP_GT_U) ? (pa >  pb) : (pa >= pb);
                        pir_val_set_u64(&r0, pir_type_scalar(PIR_SCALAR_I8), c ? 1u : 0u);
                    } else {
                        bool is_vec = (inst->ty.kind == PIR_TYPE_VECTOR);
                        uint32_t lanes = is_vec ? pir_vec_lanes(inst->ty.u.vec) : 1u;
                        PirScalarType sc = is_vec ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar;
                        unsigned bits = pir_scalar_bits(sc, it->ptr_bytes);
                        for (uint32_t l = 0u; l < lanes; ++l) {
                            uint64_t ua = read_lane_u64(&a, l, bits);
                            uint64_t ub = read_lane_u64(&b, l, bits);
                            int64_t sa = sext_u64(ua, bits), sb = sext_u64(ub, bits);
                            bool c = false;
                            if (inst->op == PIR_OP_CMP_EQ)        c = (ua == ub);
                            else if (inst->op == PIR_OP_CMP_NE)   c = (ua != ub);
                            else if (inst->op == PIR_OP_CMP_LT_S) c = (sa <  sb);
                            else if (inst->op == PIR_OP_CMP_LT_U) c = (ua <  ub);
                            else if (inst->op == PIR_OP_CMP_LE_S) c = (sa <= sb);
                            else if (inst->op == PIR_OP_CMP_LE_U) c = (ua <= ub);
                            else if (inst->op == PIR_OP_CMP_GT_S) c = (sa >  sb);
                            else if (inst->op == PIR_OP_CMP_GT_U) c = (ua >  ub);
                            else if (inst->op == PIR_OP_CMP_GE_S) c = (sa >= sb);
                            else                                  c = (ua >= ub);
                            if (is_vec) write_lane_u64(&r0, l, bits, c ? width_mask(bits) : 0u);
                            else        pir_val_set_u64(&r0, pir_type_scalar(PIR_SCALAR_I8), c ? 1u : 0u);
                        }
                    }
                    break;
                }

                case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
                case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
                case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
                case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE: {
                    PirVal a = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal b = eval_operand(it, &fr, &inst->operands[1]);
                    bool is_vec = (inst->ty.kind == PIR_TYPE_VECTOR);
                    uint32_t lanes = is_vec ? pir_vec_lanes(inst->ty.u.vec) : 1u;
                    PirScalarType sc = is_vec ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar;
                    unsigned bits = pir_scalar_bits(sc, it->ptr_bytes);
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        bool c = eval_fcmp_lane(inst->op, read_lane_f64(&a, l, sc), read_lane_f64(&b, l, sc));
                        if (is_vec) write_lane_u64(&r0, l, bits, c ? width_mask(bits) : 0u);
                        else        pir_val_set_u64(&r0, pir_type_scalar(PIR_SCALAR_I8), c ? 1u : 0u);
                    }
                    break;
                }

                case PIR_OP_SELECT: {
                    PirVal cond = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal tv   = eval_operand(it, &fr, &inst->operands[1]);
                    PirVal fv   = eval_operand(it, &fr, &inst->operands[2]);
                    if (inst->ty.kind == PIR_TYPE_VECTOR) {
                        uint32_t lanes = pir_vec_lanes(inst->ty.u.vec);
                        unsigned bits = pir_scalar_bits(pir_vec_elem_scalar(inst->ty.u.vec), it->ptr_bytes);
                        uint64_t all1 = width_mask(bits);
                        for (uint32_t l = 0u; l < lanes; ++l) {
                            uint64_t m = read_lane_u64(&cond, l, bits);
                            if (m != 0u && m != all1) {
                                pir_interp_fail(it, PIR_INTERP_UB, "invalid vector select mask lane");
                                break;
                            }
                            write_lane_u64(&r0, l, bits, m ? read_lane_u64(&tv, l, bits) : read_lane_u64(&fv, l, bits));
                        }
                    } else {
                        uint8_t cb = 0u;
                        if (check_bool_i8(it, &cond, &cb)) {
                            r0 = cb ? tv : fv;
                        }
                    }
                    break;
                }

                case PIR_OP_ZEXT: case PIR_OP_SEXT: case PIR_OP_TRUNC: {
                    PirVal src = eval_operand(it, &fr, &inst->operands[0]);
                    bool is_vec = (inst->ty.kind == PIR_TYPE_VECTOR);
                    uint32_t lanes = is_vec ? pir_vec_lanes(inst->ty.u.vec) : 1u;
                    unsigned sb = pir_scalar_bits(is_vec ? pir_vec_elem_scalar(src.ty.u.vec) : src.ty.u.scalar, it->ptr_bytes);
                    unsigned db = pir_scalar_bits(is_vec ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar, it->ptr_bytes);
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        uint64_t uv = read_lane_u64(&src, l, sb);
                        uint64_t rv = (inst->op == PIR_OP_SEXT) ? ((uint64_t)sext_u64(uv, sb) & width_mask(db))
                                                                : (uv & width_mask(db));
                        write_lane_u64(&r0, l, db, rv);
                    }
                    break;
                }

                case PIR_OP_FPEXT: case PIR_OP_FPTRUNC: {
                    PirVal src = eval_operand(it, &fr, &inst->operands[0]);
                    bool is_vec = (inst->ty.kind == PIR_TYPE_VECTOR);
                    uint32_t lanes = is_vec ? pir_vec_lanes(inst->ty.u.vec) : 1u;
                    PirScalarType ss = is_vec ? pir_vec_elem_scalar(src.ty.u.vec) : src.ty.u.scalar;
                    PirScalarType ds = is_vec ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar;
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        write_lane_f64(&r0, l, ds, read_lane_f64(&src, l, ss));
                    }
                    break;
                }

                case PIR_OP_FPTOSI: case PIR_OP_FPTOUI: {
                    PirVal src = eval_operand(it, &fr, &inst->operands[0]);
                    bool is_vec = (inst->ty.kind == PIR_TYPE_VECTOR);
                    uint32_t lanes = is_vec ? pir_vec_lanes(inst->ty.u.vec) : 1u;
                    PirScalarType ss = is_vec ? pir_vec_elem_scalar(src.ty.u.vec) : src.ty.u.scalar;
                    unsigned db = pir_scalar_bits(is_vec ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar, it->ptr_bytes);
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        uint64_t rv = 0u;
                        if (!eval_fpto_int_lane(it, inst->op, inst->flags, db, read_lane_f64(&src, l, ss), &rv)) break;
                        write_lane_u64(&r0, l, db, rv);
                    }
                    break;
                }

                case PIR_OP_SITOFP: case PIR_OP_UITOFP: {
                    PirVal src = eval_operand(it, &fr, &inst->operands[0]);
                    bool is_vec = (inst->ty.kind == PIR_TYPE_VECTOR);
                    uint32_t lanes = is_vec ? pir_vec_lanes(inst->ty.u.vec) : 1u;
                    unsigned sb = pir_scalar_bits(is_vec ? pir_vec_elem_scalar(src.ty.u.vec) : src.ty.u.scalar, it->ptr_bytes);
                    PirScalarType ds = is_vec ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar;
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        uint64_t uv = read_lane_u64(&src, l, sb);
                        if (ds == PIR_SCALAR_F32) {
                            float fv = (inst->op == PIR_OP_SITOFP) ? (float)sext_u64(uv, sb) : (float)uv;
                            memcpy(r0.bytes + l * 4u, &fv, 4u);
                        } else {
                            double dv = (inst->op == PIR_OP_SITOFP) ? (double)sext_u64(uv, sb) : (double)uv;
                            write_lane_f64(&r0, l, ds, dv);
                        }
                    }
                    break;
                }

                case PIR_OP_PTRTOINT: {
                    PirVal src = eval_operand(it, &fr, &inst->operands[0]);
                    pir_val_set_u64(&r0, inst->ty, pir_interp_ptr_to_int(it, src.ptr));
                    break;
                }

                case PIR_OP_INTTOPTR: {
                    PirVal src = eval_operand(it, &fr, &inst->operands[0]);
                    r0.ptr = pir_interp_int_to_ptr(it, pir_val_get_u64(&src));
                    break;
                }

                case PIR_OP_BITCAST: {
                    PirVal src = eval_operand(it, &fr, &inst->operands[0]);
                    bool src_is_ptr = (src.ty.kind == PIR_TYPE_SCALAR && src.ty.u.scalar == PIR_SCALAR_PTR);
                    bool dst_is_ptr = (inst->ty.kind == PIR_TYPE_SCALAR && inst->ty.u.scalar == PIR_SCALAR_PTR);
                    if (src_is_ptr && dst_is_ptr) {
                        r0.ptr = src.ptr;
                    } else if (src_is_ptr) {
                        uint64_t a = pir_interp_ptr_to_int(it, src.ptr);
                        memcpy(r0.bytes, &a, it->ptr_bytes);
                    } else if (dst_is_ptr) {
                        uint64_t a = 0u;
                        memcpy(&a, src.bytes, it->ptr_bytes);
                        r0.ptr = pir_interp_int_to_ptr(it, a);
                    } else {
                        memcpy(r0.bytes, src.bytes, sizeof(r0.bytes));
                    }
                    break;
                }

                case PIR_OP_SPLAT: {
                    PirVal sc = eval_operand(it, &fr, &inst->operands[0]);
                    uint32_t lanes = pir_vec_lanes(inst->ty.u.vec);
                    size_t esz = pir_scalar_bits(pir_vec_elem_scalar(inst->ty.u.vec), it->ptr_bytes) / 8u;
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        memcpy(r0.bytes + l * esz, sc.bytes, esz);
                    }
                    break;
                }

                case PIR_OP_EXTRACT: {
                    PirVal v = eval_operand(it, &fr, &inst->operands[0]);
                    size_t esz = pir_scalar_bits(pir_vec_elem_scalar(inst->ty.u.vec), it->ptr_bytes) / 8u;
                    memcpy(r0.bytes, v.bytes + (size_t)inst->imm_u64 * esz, esz);
                    break;
                }

                case PIR_OP_INSERT: {
                    PirVal v = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal s = eval_operand(it, &fr, &inst->operands[1]);
                    size_t esz = pir_scalar_bits(pir_vec_elem_scalar(inst->ty.u.vec), it->ptr_bytes) / 8u;
                    memcpy(r0.bytes, v.bytes, sizeof(r0.bytes));
                    memcpy(r0.bytes + (size_t)inst->imm_u64 * esz, s.bytes, esz);
                    break;
                }

                case PIR_OP_SHUFFLE: {
                    PirVal v1 = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal v2 = eval_operand(it, &fr, &inst->operands[1]);
                    uint32_t lanes = pir_vec_lanes(inst->ty.u.vec);
                    size_t esz = pir_scalar_bits(pir_vec_elem_scalar(inst->ty.u.vec), it->ptr_bytes) / 8u;
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        uint8_t idx = inst->shuffle_mask[l];
                        const uint8_t *src_b = (idx < lanes) ? (v1.bytes + idx * esz)
                                                             : (v2.bytes + (idx - lanes) * esz);
                        memcpy(r0.bytes + l * esz, src_b, esz);
                    }
                    break;
                }

                case PIR_OP_ALLOCA_PTR: {
                    PirInstId cur_iid = blk->insts.data[j];
                    uint32_t aid = fr.static_allocas[cur_iid];
                    if (aid == 0u) {
                        aid = pir_interp_alloc_new(it, (size_t)inst->imm_u64, inst->align,
                                                   false, false, false, false);
                        if (aid == 0u) break;
                        fr.static_allocas[cur_iid] = aid;
                        PIR_VEC_PUSH(&fr.stack_allocs, aid);
                    }
                    r0.ptr.alloc_id = aid;
                    r0.ptr.offset   = 0;
                    break;
                }

                case PIR_OP_ALLOCA_DYN: {
                    PirVal szv = eval_operand(it, &fr, &inst->operands[0]);
                    uint64_t sz = pir_val_get_u64(&szv);
                    uint32_t aid = pir_interp_alloc_new(it, (size_t)sz, inst->align,
                                                        false, false, true, false);
                    if (aid == 0u) break;
                    PIR_VEC_PUSH(&fr.dyn_stack_allocs, aid);
                    r0.ptr.alloc_id = aid;
                    r0.ptr.offset   = 0;
                    break;
                }

                case PIR_OP_STACKSAVE: {
                    /* Encode frame_id + monotonic token_id inside a sentinel pointer. */
                    uint32_t tid = ++fr.next_save_id;
                    StackSaveMark m = { tid, fr.dyn_stack_allocs.len };
                    PIR_VEC_PUSH(&fr.active_saves, m);
                    r0.ptr.alloc_id = 0x80000000u | (fr.frame_id & 0x7FFFFFFFu);
                    r0.ptr.offset   = (int64_t)tid;
                    break;
                }

                case PIR_OP_STACKRESTORE: {
                    PirVal sp = eval_operand(it, &fr, &inst->operands[0]);
                    uint32_t expected_tag = 0x80000000u | (fr.frame_id & 0x7FFFFFFFu);
                    if (sp.ptr.alloc_id != expected_tag || sp.ptr.offset <= 0 ||
                        sp.ptr.offset > (int64_t)UINT32_MAX) {
                        pir_interp_fail(it, PIR_INTERP_UB, "invalid stackrestore token");
                        break;
                    }
                    uint32_t tid = (uint32_t)sp.ptr.offset;
                    uint32_t found_idx = PIR_INVALID_ID;
                    for (uint32_t k = 0u; k < fr.active_saves.len; ++k) {
                        if (fr.active_saves.data[k].token_id == tid) {
                            found_idx = k;
                            break;
                        }
                    }
                    if (found_idx == PIR_INVALID_ID) {
                        pir_interp_fail(it, PIR_INTERP_UB, "stale or invalid stackrestore token");
                        break;
                    }
                    uint32_t mark = fr.active_saves.data[found_idx].dyn_mark;
                    for (uint32_t k = mark; k < fr.dyn_stack_allocs.len; ++k) {
                        it->allocs.data[fr.dyn_stack_allocs.data[k]].alive = false;
                    }
                    fr.dyn_stack_allocs.len = mark;
                    /* Keep found_idx valid for repeated restores, invalidate any inner saves. */
                    fr.active_saves.len = found_idx + 1u;
                    break;
                }

                case PIR_OP_PTR_ADD: {
                    PirVal base = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal offv = eval_operand(it, &fr, &inst->operands[1]);
                    unsigned obits = pir_scalar_bits(inst->ty.u.scalar, it->ptr_bytes);
                    int64_t delta = sext_u64(pir_val_get_u64(&offv), obits);
                    if ((base.ptr.alloc_id & 0x80000000u) != 0u) {
                        /* Never allow ptr_add on a stacksave token to forge another stacksave token. */
                        if (inst->flags & PIR_FLAG_INBOUNDS) {
                            pir_interp_fail(it, PIR_INTERP_UB, "ptr_add [inbounds] on stacksave token");
                            break;
                        }
                        uint64_t raw = pir_interp_ptr_to_int(it, base.ptr) + (uint64_t)delta;
                        r0.ptr.alloc_id = 0u;
                        r0.ptr.offset   = (it->ptr_bytes == 4u)
                                          ? (int64_t)(int32_t)(uint32_t)raw
                                          : (int64_t)raw;
                        break;
                    }
                    int64_t next_off = (int64_t)((uint64_t)base.ptr.offset + (uint64_t)delta);
                    if (inst->flags & PIR_FLAG_INBOUNDS) {
                        if (base.ptr.alloc_id == 0u) {
                            if (base.ptr.offset != 0 || delta != 0) {
                                pir_interp_fail(it, PIR_INTERP_UB, "ptr_add [inbounds] on null/invalid pointer");
                                break;
                            }
                        } else if (base.ptr.alloc_id >= it->allocs.len ||
                                   !it->allocs.data[base.ptr.alloc_id].alive) {
                            pir_interp_fail(it, PIR_INTERP_UB, "ptr_add [inbounds] on dead allocation");
                            break;
                        } else if (it->allocs.data[base.ptr.alloc_id].is_func) {
                            pir_interp_fail(it, PIR_INTERP_UB, "ptr_add [inbounds] on function pointer");
                            break;
                        } else {
                            size_t sz = it->allocs.data[base.ptr.alloc_id].size;
                            if (base.ptr.offset < 0 || (uint64_t)base.ptr.offset > sz ||
                                next_off < 0 || (uint64_t)next_off > sz) {
                                pir_interp_fail(it, PIR_INTERP_UB,
                                                "ptr_add [inbounds] out of object bounds (offset %" PRId64 ", size %zu)",
                                                next_off, sz);
                                break;
                            }
                        }
                    } else if (it->ptr_bytes == 4u) {
                        next_off = (int64_t)(int32_t)((uint32_t)base.ptr.offset + (uint32_t)delta);
                    }
                    r0.ptr.alloc_id = base.ptr.alloc_id;
                    r0.ptr.offset   = next_off;
                    break;
                }

                case PIR_OP_LOAD: {
                    PirVal p = eval_operand(it, &fr, &inst->operands[0]);
                    if (inst->ty.kind == PIR_TYPE_SCALAR && inst->ty.u.scalar == PIR_SCALAR_PTR) {
                        pir_interp_mem_read_ptr(it, p.ptr, &r0.ptr, inst->align);
                    } else {
                        pir_interp_mem_read(it, p.ptr, r0.bytes, type_byte_size(inst->ty, it->ptr_bytes), inst->align);
                    }
                    break;
                }

                case PIR_OP_STORE: {
                    PirVal val = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal p   = eval_operand(it, &fr, &inst->operands[1]);
                    if (inst->ty.kind == PIR_TYPE_SCALAR && inst->ty.u.scalar == PIR_SCALAR_PTR) {
                        pir_interp_mem_write_ptr(it, p.ptr, val.ptr, inst->align);
                    } else {
                        pir_interp_mem_write(it, p.ptr, val.bytes, type_byte_size(inst->ty, it->ptr_bytes), inst->align);
                    }
                    break;
                }

                case PIR_OP_ATOMIC_LOAD: {
                    PirVal p = eval_operand(it, &fr, &inst->operands[0]);
                    uint32_t nat = (uint32_t)type_byte_size(inst->ty, it->ptr_bytes);
                    if (inst->ty.u.scalar == PIR_SCALAR_PTR) {
                        pir_interp_mem_read_ptr(it, p.ptr, &r0.ptr, nat);
                    } else {
                        pir_interp_mem_read(it, p.ptr, r0.bytes, nat, nat);
                    }
                    break;
                }

                case PIR_OP_ATOMIC_STORE: {
                    PirVal val = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal p   = eval_operand(it, &fr, &inst->operands[1]);
                    uint32_t nat = (uint32_t)type_byte_size(inst->ty, it->ptr_bytes);
                    if (inst->ty.u.scalar == PIR_SCALAR_PTR) {
                        pir_interp_mem_write_ptr(it, p.ptr, val.ptr, nat);
                    } else {
                        pir_interp_mem_write(it, p.ptr, val.bytes, nat, nat);
                    }
                    break;
                }

                case PIR_OP_ATOMIC_RMW: {
                    PirVal p   = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal val = eval_operand(it, &fr, &inst->operands[1]);
                    uint32_t nat = (uint32_t)type_byte_size(inst->ty, it->ptr_bytes);
                    if (inst->ty.u.scalar == PIR_SCALAR_PTR) {
                        if (!pir_interp_mem_read_ptr(it, p.ptr, &r0.ptr, nat)) break;
                        if (inst->rmw_op == PIR_RMW_XCHG) {
                            pir_interp_mem_write_ptr(it, p.ptr, val.ptr, nat);
                        } else {
                            pir_interp_fail(it, PIR_INTERP_UB, "non-xchg atomic_rmw on ptr");
                        }
                    } else {
                        if (!pir_interp_mem_read(it, p.ptr, r0.bytes, nat, nat)) break;
                        unsigned bits = nat * 8u;
                        uint64_t old_v = pir_val_get_u64(&r0);
                        uint64_t arg_v = pir_val_get_u64(&val);
                        uint64_t nxt = 0u;
                        switch (inst->rmw_op) {
                            case PIR_RMW_ADD:  nxt = old_v + arg_v; break;
                            case PIR_RMW_SUB:  nxt = old_v - arg_v; break;
                            case PIR_RMW_AND:  nxt = old_v & arg_v; break;
                            case PIR_RMW_OR:   nxt = old_v | arg_v; break;
                            case PIR_RMW_XOR:  nxt = old_v ^ arg_v; break;
                            case PIR_RMW_XCHG: nxt = arg_v;         break;
                        }
                        PirVal nv;
                        pir_val_set_u64(&nv, inst->ty, nxt & width_mask(bits));
                        pir_interp_mem_write(it, p.ptr, nv.bytes, nat, nat);
                    }
                    break;
                }

                case PIR_OP_ATOMIC_CMPXCHG: {
                    PirVal p    = eval_operand(it, &fr, &inst->operands[0]);
                    PirVal expv = eval_operand(it, &fr, &inst->operands[1]);
                    PirVal newv = eval_operand(it, &fr, &inst->operands[2]);
                    uint32_t nat = (uint32_t)type_byte_size(inst->ty, it->ptr_bytes);
                    if (!pir_interp_check_access(it, p.ptr, nat, nat, true, true)) break;
                    if (inst->ty.u.scalar == PIR_SCALAR_PTR) {
                        if (!pir_interp_mem_read_ptr(it, p.ptr, &r0.ptr, nat)) break;
                        bool eq = (pir_interp_ptr_to_int(it, r0.ptr) == pir_interp_ptr_to_int(it, expv.ptr));
                        if (eq) pir_interp_mem_write_ptr(it, p.ptr, newv.ptr, nat);
                        pir_val_set_u64(&r1, pir_type_scalar(PIR_SCALAR_I8), eq ? 1u : 0u);
                    } else {
                        if (!pir_interp_mem_read(it, p.ptr, r0.bytes, nat, nat)) break;
                        bool eq = (memcmp(r0.bytes, expv.bytes, nat) == 0);
                        if (eq) pir_interp_mem_write(it, p.ptr, newv.bytes, nat, nat);
                        pir_val_set_u64(&r1, pir_type_scalar(PIR_SCALAR_I8), eq ? 1u : 0u);
                    }
                    break;
                }

                case PIR_OP_FENCE:
                    break; /* Single-threaded sequential consistency */

                case PIR_OP_VA_START: {
                    PirVal ap = eval_operand(it, &fr, &inst->operands[0]);
                    bool is_sysv_x64 = (it->target && it->target->arch == PIR_ARCH_X86_64 &&
                                        it->target->os != PIR_OS_WINDOWS);
                    bool is_aapcs64_lin = (it->target && it->target->arch == PIR_ARCH_ARM64 &&
                                           it->target->os != PIR_OS_MACOS);
                    bool is_darwin_arm64 = (it->target && it->target->arch == PIR_ARCH_ARM64 &&
                                            it->target->os == PIR_OS_MACOS);

                    if (is_sysv_x64) {
                        uint32_t gp_used = 0u, fp_used = 0u;
                        for (uint32_t k = 0u; k < fn->param_count; ++k) {
                            if (k == 0u && fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG) continue;
                            if (fn->params[k].abi_byval_size > 0u || fn->params[k].abi_force_stack) continue;
                            PirType pt = fn->params[k].type;
                            if (pir_type_is_float(pt) || pt.kind == PIR_TYPE_VECTOR) {
                                if (fp_used < 8u) fp_used++;
                            } else {
                                if (gp_used < 6u) gp_used++;
                            }
                        }
                        uint32_t init_gp_off = gp_used * 8u;
                        uint32_t init_fp_off = 48u + fp_used * 16u;
                        uint32_t rsa_aid = pir_interp_alloc_new(it, 176u, 16u, false, false, false, true);
                        size_t oaa_cap = 64u;
                        uint32_t oaa_align = 32u;
                        for (uint32_t k = 0u; k < fr.vararg_count; ++k) {
                            const PirCallArgMeta *mt = fr.vararg_meta ? &fr.vararg_meta[k] : NULL;
                            uint32_t arg_al = 8u;
                            if (mt && mt->byval_align > arg_al) arg_al = mt->byval_align;
                            if (mt && mt->stack_align > arg_al) arg_al = mt->stack_align;
                            if (arg_al > oaa_align && pir_is_pow2_u32(arg_al) && arg_al <= (1u << 16)) {
                                oaa_align = arg_al;
                            }
                            size_t base_sz = (mt && mt->byval_size > 0u) ? (size_t)mt->byval_size : 64u;
                            oaa_cap += base_sz + (size_t)arg_al + 64u;
                        }
                        uint32_t oaa_aid = pir_interp_alloc_new(it, oaa_cap, oaa_align, false, false, false, true);
                        if (rsa_aid == 0u || oaa_aid == 0u) break;
                        PIR_VEC_PUSH(&fr.stack_allocs, rsa_aid);
                        PIR_VEC_PUSH(&fr.stack_allocs, oaa_aid);

                        uint64_t oaa_base_va = it->allocs.data[oaa_aid].base_addr;
                        uint32_t gp_off = init_gp_off;
                        uint32_t fp_off = init_fp_off;
                        size_t oaa_off = 0u;
                        for (uint32_t k = 0u; k < fr.vararg_count; ++k) {
                            const PirVal *va = &fr.varargs[k];
                            const PirCallArgMeta *mt = fr.vararg_meta ? &fr.vararg_meta[k] : NULL;
                            if (mt && mt->byval_size > 0u) {
                                size_t al = (mt->byval_align > 8u) ? (size_t)mt->byval_align : 8u;
                                uint64_t cur_va = oaa_base_va + (uint64_t)oaa_off;
                                uint64_t al_va  = (cur_va + (uint64_t)al - 1u) & ~((uint64_t)al - 1u);
                                oaa_off = (size_t)(al_va - oaa_base_va);
                                PirPtr dst = { oaa_aid, (int64_t)oaa_off };
                                pir_interp_mem_copy(it, dst, va->ptr, (size_t)mt->byval_size, false);
                                oaa_off += ((size_t)mt->byval_size + 7u) & ~(size_t)7u;
                                continue;
                            }
                            bool is_fp = (pir_type_is_float(va->ty) || va->ty.kind == PIR_TYPE_VECTOR);
                            bool is_f32 = (va->ty.kind == PIR_TYPE_SCALAR && va->ty.u.scalar == PIR_SCALAR_F32 &&
                                           !(mt && mt->no_f32_promote));
                            size_t vsz = is_f32 ? 8u : type_byte_size(va->ty, it->ptr_bytes);
                            if (vsz == 0u) vsz = 8u;
                            if (is_fp) {
                                uint32_t rstep = (vsz > 16u) ? (uint32_t)((vsz + 15u) & ~(size_t)15u) : 16u;
                                PirPtr dst;
                                if (!(mt && mt->force_stack) && fp_off + rstep <= 176u) {
                                    dst.alloc_id = rsa_aid;
                                    dst.offset   = (int64_t)fp_off;
                                    fp_off += rstep;
                                } else {
                                    size_t al = (vsz > 8u) ? ((vsz > 32u) ? 32u : vsz) : 8u;
                                    if (mt && mt->stack_align > al) al = mt->stack_align;
                                    uint64_t cur_va = oaa_base_va + (uint64_t)oaa_off;
                                    uint64_t al_va  = (cur_va + (uint64_t)al - 1u) & ~((uint64_t)al - 1u);
                                    oaa_off = (size_t)(al_va - oaa_base_va);
                                    dst.alloc_id = oaa_aid;
                                    dst.offset   = (int64_t)oaa_off;
                                    oaa_off += (vsz + 7u) & ~(size_t)7u;
                                }
                                if (is_f32) {
                                    float fv = 0.0f;
                                    memcpy(&fv, va->bytes, 4u);
                                    double dv = (double)fv;
                                    pir_interp_mem_write(it, dst, &dv, 8u, 1u);
                                } else {
                                    pir_interp_mem_write(it, dst, va->bytes, vsz, 1u);
                                }
                            } else {
                                PirPtr dst;
                                if (!(mt && mt->force_stack) && gp_off + 8u <= 48u) {
                                    dst.alloc_id = rsa_aid;
                                    dst.offset   = (int64_t)gp_off;
                                    gp_off += 8u;
                                } else {
                                    size_t al = (mt && mt->stack_align > 8u) ? (size_t)mt->stack_align : 8u;
                                    uint64_t cur_va = oaa_base_va + (uint64_t)oaa_off;
                                    uint64_t al_va  = (cur_va + (uint64_t)al - 1u) & ~((uint64_t)al - 1u);
                                    oaa_off = (size_t)(al_va - oaa_base_va);
                                    dst.alloc_id = oaa_aid;
                                    dst.offset   = (int64_t)oaa_off;
                                    oaa_off += 8u;
                                }
                                if (va->ty.kind == PIR_TYPE_SCALAR && va->ty.u.scalar == PIR_SCALAR_PTR) {
                                    pir_interp_mem_write_ptr(it, dst, va->ptr, 1u);
                                } else {
                                    pir_interp_mem_write(it, dst, va->bytes, vsz <= 8u ? vsz : 8u, 1u);
                                }
                            }
                        }
                        PirPtr p0 = ap.ptr, p4, p8, p16;
                        if (!pir_ptr_offset_checked(it, ap.ptr, 4, &p4) ||
                            !pir_ptr_offset_checked(it, ap.ptr, 8, &p8) ||
                            !pir_ptr_offset_checked(it, ap.ptr, 16, &p16)) break;
                        PirPtr oaa_ptr = { oaa_aid, 0 };
                        PirPtr rsa_ptr = { rsa_aid, 0 };
                        pir_interp_mem_write(it, p0, &init_gp_off, 4u, 1u);
                        pir_interp_mem_write(it, p4, &init_fp_off, 4u, 1u);
                        pir_interp_mem_write_ptr(it, p8, oaa_ptr, 1u);
                        pir_interp_mem_write_ptr(it, p16, rsa_ptr, 1u);
                    } else if (is_aapcs64_lin) {
                        uint32_t gp_used = 0u, fp_used = 0u;
                        for (uint32_t k = 0u; k < fn->param_count; ++k) {
                            if (k == 0u && (fn->cabi_ret.kind == PIR_CABI_RET_SRET_X8 ||
                                            fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG)) continue;
                            PirType pt = fn->params[k].type;
                            if (pir_type_is_float(pt) || pt.kind == PIR_TYPE_VECTOR) {
                                if (fp_used < 8u) fp_used++;
                            } else {
                                if (gp_used < 8u) gp_used++;
                            }
                        }
                        int32_t init_gr = -(int32_t)((8u - gp_used) * 8u);
                        int32_t init_vr = -(int32_t)((8u - fp_used) * 16u);
                        uint32_t gr_aid = pir_interp_alloc_new(it, 64u, 16u, false, false, false, true);
                        uint32_t vr_aid = pir_interp_alloc_new(it, 128u, 16u, false, false, false, true);
                        size_t stk_cap = 32u + (size_t)fr.vararg_count * 64u;
                        uint32_t stk_aid = pir_interp_alloc_new(it, stk_cap, 16u, false, false, false, true);
                        if (gr_aid == 0u || vr_aid == 0u || stk_aid == 0u) break;
                        PIR_VEC_PUSH(&fr.stack_allocs, gr_aid);
                        PIR_VEC_PUSH(&fr.stack_allocs, vr_aid);
                        PIR_VEC_PUSH(&fr.stack_allocs, stk_aid);

                        int32_t cur_gr = init_gr;
                        int32_t cur_vr = init_vr;
                        size_t stk_off = 0u;
                        for (uint32_t k = 0u; k < fr.vararg_count; ++k) {
                            const PirVal *va = &fr.varargs[k];
                            const PirCallArgMeta *mt = fr.vararg_meta ? &fr.vararg_meta[k] : NULL;
                            bool is_fp = (pir_type_is_float(va->ty) || va->ty.kind == PIR_TYPE_VECTOR);
                            bool is_f32 = (va->ty.kind == PIR_TYPE_SCALAR && va->ty.u.scalar == PIR_SCALAR_F32 &&
                                           !(mt && mt->no_f32_promote));
                            size_t vsz = is_f32 ? 8u : type_byte_size(va->ty, it->ptr_bytes);
                            if (vsz == 0u) vsz = 8u;
                            if (is_fp) {
                                int32_t rstep = (vsz > 16u) ? (int32_t)((vsz + 15u) & ~(size_t)15u) : 16;
                                PirPtr dst;
                                if (cur_vr + rstep <= 0) {
                                    dst.alloc_id = vr_aid;
                                    dst.offset   = (int64_t)(128 + cur_vr);
                                    cur_vr += rstep;
                                } else {
                                    cur_vr = 0;
                                    size_t al = (vsz > 8u) ? 16u : 8u;
                                    if (mt && mt->stack_align > al) al = mt->stack_align;
                                    stk_off = (stk_off + al - 1u) & ~(al - 1u);
                                    dst.alloc_id = stk_aid;
                                    dst.offset   = (int64_t)stk_off;
                                    stk_off += (vsz + 7u) & ~(size_t)7u;
                                }
                                if (is_f32) {
                                    float fv = 0.0f;
                                    memcpy(&fv, va->bytes, 4u);
                                    double dv = (double)fv;
                                    pir_interp_mem_write(it, dst, &dv, 8u, 1u);
                                } else {
                                    pir_interp_mem_write(it, dst, va->bytes, vsz, 1u);
                                }
                            } else {
                                PirPtr dst;
                                if (cur_gr + 8 <= 0) {
                                    dst.alloc_id = gr_aid;
                                    dst.offset   = (int64_t)(64 + cur_gr);
                                    cur_gr += 8;
                                } else {
                                    cur_gr = 0;
                                    size_t al = (mt && mt->stack_align > 8u) ? (size_t)mt->stack_align : 8u;
                                    stk_off = (stk_off + al - 1u) & ~(al - 1u);
                                    dst.alloc_id = stk_aid;
                                    dst.offset   = (int64_t)stk_off;
                                    stk_off += 8u;
                                }
                                if (va->ty.kind == PIR_TYPE_SCALAR && va->ty.u.scalar == PIR_SCALAR_PTR) {
                                    pir_interp_mem_write_ptr(it, dst, va->ptr, 1u);
                                } else {
                                    pir_interp_mem_write(it, dst, va->bytes, vsz <= 8u ? vsz : 8u, 1u);
                                }
                            }
                        }
                        PirPtr p0 = ap.ptr, p8, p16, p24, p28;
                        if (!pir_ptr_offset_checked(it, ap.ptr, 8, &p8) ||
                            !pir_ptr_offset_checked(it, ap.ptr, 16, &p16) ||
                            !pir_ptr_offset_checked(it, ap.ptr, 24, &p24) ||
                            !pir_ptr_offset_checked(it, ap.ptr, 28, &p28)) break;
                        PirPtr stk_ptr = { stk_aid, 0 };
                        PirPtr gr_top  = { gr_aid, 64 };
                        PirPtr vr_top  = { vr_aid, 128 };
                        pir_interp_mem_write_ptr(it, p0, stk_ptr, 1u);
                        pir_interp_mem_write_ptr(it, p8, gr_top, 1u);
                        pir_interp_mem_write_ptr(it, p16, vr_top, 1u);
                        pir_interp_mem_write(it, p24, &init_gr, 4u, 1u);
                        pir_interp_mem_write(it, p28, &init_vr, 4u, 1u);
                    } else {
                        size_t slot_sz = (it->ptr_bytes == 4u) ? 4u : 8u;
                        size_t cap = 32u + (size_t)fr.vararg_count * 64u;
                        uint32_t va_aid = pir_interp_alloc_new(it, cap, 16u, false, false, false, true);
                        if (va_aid == 0u) break;
                        PIR_VEC_PUSH(&fr.stack_allocs, va_aid);
                        size_t off = 0u;
                        for (uint32_t k = 0u; k < fr.vararg_count; ++k) {
                            const PirVal *va = &fr.varargs[k];
                            const PirCallArgMeta *mt = fr.vararg_meta ? &fr.vararg_meta[k] : NULL;
                            bool is_f32 = (va->ty.kind == PIR_TYPE_SCALAR && va->ty.u.scalar == PIR_SCALAR_F32 &&
                                           !(mt && mt->no_f32_promote));
                            size_t vsz = is_f32 ? 8u : type_byte_size(va->ty, it->ptr_bytes);
                            if (vsz == 0u) vsz = slot_sz;
                            if (is_darwin_arm64) {
                                size_t al = (vsz > 8u) ? 16u : 8u;
                                if (mt && mt->stack_align > al) al = mt->stack_align;
                                if (al > 8u) {
                                    off = (off + al - 1u) & ~(al - 1u);
                                }
                            }
                            PirPtr dst = { va_aid, (int64_t)off };
                            if (is_f32) {
                                float fv = 0.0f;
                                memcpy(&fv, va->bytes, 4u);
                                double dv = (double)fv;
                                pir_interp_mem_write(it, dst, &dv, 8u, 1u);
                            } else if (va->ty.kind == PIR_TYPE_SCALAR && va->ty.u.scalar == PIR_SCALAR_PTR) {
                                pir_interp_mem_write_ptr(it, dst, va->ptr, 1u);
                            } else {
                                pir_interp_mem_write(it, dst, va->bytes, vsz, 1u);
                            }
                            off += (vsz + slot_sz - 1u) & ~(slot_sz - 1u);
                        }
                        PirPtr va_ptr = { va_aid, 0 };
                        pir_interp_mem_write_ptr(it, ap.ptr, va_ptr, 1u);
                    }
                    break;
                }

                case PIR_OP_CALL: {
                    PirFuncId cid = inst->operands[0].u.sym.id;
                    uint32_t nargs = inst->operand_count - 1u;
                    PirVal *cargs = nargs ? (PirVal *)malloc(nargs * sizeof(PirVal)) : NULL;
                    PirCallArgMeta *cmeta = nargs ? (PirCallArgMeta *)calloc(nargs, sizeof(PirCallArgMeta)) : NULL;
                    if (nargs && (!cargs || !cmeta)) abort();
                    for (uint32_t k = 0u; k < nargs; ++k) {
                        const PirOperand *aop = &inst->operands[1u + k];
                        cargs[k] = eval_operand(it, &fr, aop);
                        cmeta[k].byval_size     = aop->call_byval_size;
                        cmeta[k].byval_align    = aop->call_byval_align;
                        cmeta[k].stack_align    = aop->call_stack_align;
                        cmeta[k].force_stack    = aop->call_force_stack;
                        cmeta[k].no_f32_promote = aop->call_no_f32_promote;
                    }
                    if (!it->mod->funcs.data[cid].is_defined &&
                        strcmp(it->mod->funcs.data[cid].name, "__pir_setjmp") == 0 &&
                        nargs >= 1u) {
                        fr.has_setjmp = true;
                        fr.setjmp_jb  = cargs[0].ptr;
                        fr.setjmp_blk = cur_blk;
                        fr.setjmp_j   = j;
                        fr.setjmp_res = inst->results[0];
                        pir_val_set_u64(&r0, inst->ty, 0u);
                    } else {
                        exec_call_target(it, cid, cargs, cmeta, nargs, &r0);
                    }
                    free(cmeta);
                    free(cargs);
                    break;
                }

                case PIR_OP_CALL_INDIRECT: {
                    PirVal fn_ptr = eval_operand(it, &fr, &inst->operands[0]);
                    if (fn_ptr.ptr.alloc_id == 0u || fn_ptr.ptr.alloc_id >= it->allocs.len ||
                        !it->allocs.data[fn_ptr.ptr.alloc_id].is_func || fn_ptr.ptr.offset != 0) {
                        pir_interp_fail(it, PIR_INTERP_UB, "call_indirect through non-function pointer");
                        break;
                    }
                    PirFuncId cid = it->allocs.data[fn_ptr.ptr.alloc_id].func_id;
                    const PirFunction *callee = &it->mod->funcs.data[cid];
                    const PirSigDef *sig = (inst->sig_id < it->mod->sigs.len)
                                           ? &it->mod->sigs.data[inst->sig_id] : NULL;
                    uint32_t nargs = inst->operand_count - 1u;
                    if ((!callee->is_variadic && nargs != callee->param_count) ||
                        (callee->is_variadic && nargs < callee->param_count)) {
                        pir_interp_fail(it, PIR_INTERP_UB,
                                        "call_indirect argument count mismatch for '@%s' (expected %s%u, got %u)",
                                        callee->name, callee->is_variadic ? "at least " : "",
                                        callee->param_count, nargs);
                        break;
                    }
                    if (sig) {
                        if (sig->cc != callee->cc ||
                            sig->is_variadic != callee->is_variadic ||
                            !pir_type_eq(sig->ret_type, callee->ret_type) ||
                            sig->cabi_ret.kind != callee->cabi_ret.kind ||
                            sig->cabi_ret.reg_count != callee->cabi_ret.reg_count ||
                            sig->param_count != callee->param_count) {
                            pir_interp_fail(it, PIR_INTERP_UB,
                                            "call_indirect signature mismatch for '@%s'", callee->name);
                            break;
                        }
                        bool param_ok = true;
                        for (uint32_t k = 0u; k < callee->param_count; ++k) {
                            if (!pir_type_eq(sig->params[k].type, callee->params[k].type) ||
                                sig->params[k].abi_byval_size != callee->params[k].abi_byval_size ||
                                sig->params[k].abi_force_stack != callee->params[k].abi_force_stack) {
                                param_ok = false;
                                break;
                            }
                        }
                        if (!param_ok) {
                            pir_interp_fail(it, PIR_INTERP_UB,
                                            "call_indirect parameter type mismatch for '@%s'", callee->name);
                            break;
                        }
                    }
                    PirVal *cargs = nargs ? (PirVal *)malloc(nargs * sizeof(PirVal)) : NULL;
                    PirCallArgMeta *cmeta = nargs ? (PirCallArgMeta *)calloc(nargs, sizeof(PirCallArgMeta)) : NULL;
                    if (nargs && (!cargs || !cmeta)) abort();
                    for (uint32_t k = 0u; k < nargs; ++k) {
                        const PirOperand *aop = &inst->operands[1u + k];
                        cargs[k] = eval_operand(it, &fr, aop);
                        cmeta[k].byval_size     = aop->call_byval_size;
                        cmeta[k].byval_align    = aop->call_byval_align;
                        cmeta[k].stack_align    = aop->call_stack_align;
                        cmeta[k].force_stack    = aop->call_force_stack;
                        cmeta[k].no_f32_promote = aop->call_no_f32_promote;
                    }
                    exec_call_target(it, cid, cargs, cmeta, nargs, &r0);
                    free(cmeta);
                    free(cargs);
                    break;
                }

                case PIR_OP_JMP:
                    transfer_branch(it, &fr, &inst->targets[0], &cur_blk);
                    break;

                case PIR_OP_BR: {
                    PirVal cond = eval_operand(it, &fr, &inst->operands[0]);
                    uint8_t cb = 0u;
                    if (check_bool_i8(it, &cond, &cb)) {
                        transfer_branch(it, &fr, cb ? &inst->targets[0] : &inst->targets[1], &cur_blk);
                    }
                    break;
                }

                case PIR_OP_SWITCH: {
                    PirVal sv = eval_operand(it, &fr, &inst->operands[0]);
                    unsigned bits = pir_scalar_bits(inst->ty.u.scalar, it->ptr_bytes);
                    uint64_t val = pir_val_get_u64(&sv) & width_mask(bits);
                    const PirBranchTarget *chosen = &inst->targets[0];
                    for (uint32_t c = 0u; c < inst->switch_case_count; ++c) {
                        uint64_t cv = inst->switch_cases[c].is_negative
                                      ? (UINT64_C(0) - inst->switch_cases[c].case_lo)
                                      : inst->switch_cases[c].case_lo;
                        if ((cv & width_mask(bits)) == val) {
                            chosen = &inst->switch_cases[c].target;
                            break;
                        }
                    }
                    transfer_branch(it, &fr, chosen, &cur_blk);
                    break;
                }

                case PIR_OP_RET:
                    if (inst->operand_count == 1u && out_ret) {
                        *out_ret = eval_operand(it, &fr, &inst->operands[0]);
                    }
                    returned = true;
                    break;

                case PIR_OP_TRAP:
                    pir_interp_fail(it, PIR_INTERP_TRAP, "executed trap instruction");
                    break;

                case PIR_OP_UNREACHABLE:
                    pir_interp_fail(it, PIR_INTERP_UB, "executed unreachable instruction");
                    break;

                /* HIR-only opcodes (rejected by LIR verifier before execution). */
                case PIR_OP_ALLOCA:
                case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR:
                case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
                case PIR_OP_VA_ARG: case PIR_OP_VA_ARG_BUF:
                case PIR_OP_VA_COPY: case PIR_OP_VA_END:
                case PIR_OP_INVOKE: case PIR_OP_INVOKE_INDIRECT: case PIR_OP_RESUME:
                    pir_interp_fail(it, PIR_INTERP_ERR, "HIR-only opcode reached LIR interpreter");
                    break;
            }

            if (it->unwinding) {
                if (fr.has_setjmp &&
                    fr.setjmp_jb.alloc_id == it->unwind_target_jb.alloc_id &&
                    fr.setjmp_jb.offset   == it->unwind_target_jb.offset) {
                    it->unwinding = false;
                    if (fr.setjmp_res != PIR_INVALID_ID) {
                        pir_val_set_u64(&fr.vals[fr.setjmp_res], pir_type_scalar(PIR_SCALAR_I32), 1u);
                    }
                    cur_blk = fr.setjmp_blk;
                    start_j = fr.setjmp_j + 1u;
                }
                break;
            }
            if (it->status == PIR_INTERP_OK && !it->exited) {
                if (inst->results[0] != PIR_INVALID_ID) fr.vals[inst->results[0]] = r0;
                if (inst->results[1] != PIR_INVALID_ID) fr.vals[inst->results[1]] = r1;
            }
            if (pir_op_is_terminator(inst->op)) {
                break;
            }
        }
    }

    /* Reclaim all stack allocations of this frame (catches dangling stack ptr UAF!). */
    for (uint32_t k = 0u; k < fr.stack_allocs.len; ++k) {
        it->allocs.data[fr.stack_allocs.data[k]].alive = false;
    }
    for (uint32_t k = 0u; k < fr.dyn_stack_allocs.len; ++k) {
        it->allocs.data[fr.dyn_stack_allocs.data[k]].alive = false;
    }
    PIR_VEC_FREE(&fr.stack_allocs);
    PIR_VEC_FREE(&fr.dyn_stack_allocs);
    PIR_VEC_FREE(&fr.active_saves);
    free(fr.static_allocas);
    free(fr.vals);
    it->call_depth--;
    return it->status == PIR_INTERP_OK;
}

/* =========================================================================
 * Module Initialization & Entry Point
 * ========================================================================= */

static size_t compute_global_size(const PirGlobal *g, uint8_t ptr_bytes) {
    if (g->has_type && (g->type.kind == PIR_TYPE_SCALAR || g->type.kind == PIR_TYPE_VECTOR)) {
        size_t sz = type_byte_size(g->type, ptr_bytes);
        return sz > 0u ? sz : 1u;
    }
    uint64_t total = 0u;
    for (uint32_t k = 0u; k < g->item_count; ++k) {
        const PirGlobalItem *gi = &g->items[k];
        uint64_t add = 0u;
        switch (gi->kind) {
            case PIR_GITEM_BYTES: add = gi->len; break;
            case PIR_GITEM_ZERO:  add = gi->len ? gi->len : 1u; break;
            case PIR_GITEM_INT:   add = 1u; break;
            case PIR_GITEM_FLOAT: add = 8u; break;
            case PIR_GITEM_RELOC: add = ptr_bytes; break;
        }
        if (add > PIR_MAX_ALLOC_SIZE || total > PIR_MAX_ALLOC_SIZE - add) {
            return (size_t)(PIR_MAX_ALLOC_SIZE + 1u);
        }
        total += add;
    }
    return total > 0u ? (size_t)total : 1u;
}

static bool init_globals(PirInterp *it) {
    const PirModule *mod = it->mod;
    /* Reserve AllocId 0 as NULL sentinel. */
    PirAlloc null_alloc;
    memset(&null_alloc, 0, sizeof(null_alloc));
    PIR_VEC_PUSH(&it->allocs, null_alloc);

    /* Allocate 1-byte function objects first so relocs can reference them. */
    it->func_allocs = (uint32_t *)calloc(mod->funcs.len + 1u, sizeof(uint32_t));
    if (!it->func_allocs) abort();
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        const PirFunction *fn = &mod->funcs.data[i];
        if (fn->linkage == PIR_LINK_EXTERN && fn->is_weak_extern) {
            it->func_allocs[i] = 0u;
            continue;
        }
        uint32_t aid = pir_interp_alloc_new(it, 1u, 16u, true, false, false, true);
        if (aid == 0u) return false;
        it->allocs.data[aid].is_func = true;
        it->allocs.data[aid].func_id = i;
        it->func_allocs[i] = aid;
    }

    /* Pass 1: allocate storage for every global. */
    it->global_allocs = (uint32_t *)calloc(mod->globals.len + 1u, sizeof(uint32_t));
    if (!it->global_allocs) abort();
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        if (g->linkage == PIR_LINK_EXTERN && g->is_weak_extern) {
            it->global_allocs[i] = 0u;
            continue;
        }
        size_t sz = compute_global_size(g, it->ptr_bytes);
        uint32_t al = g->align;
        if (al == 0u) {
            if (g->has_type && (g->type.kind == PIR_TYPE_SCALAR || g->type.kind == PIR_TYPE_VECTOR)) {
                size_t nat = type_byte_size(g->type, it->ptr_bytes);
                al = (nat > 0u) ? (uint32_t)nat : 1u;
            } else {
                al = 1u;
            }
        }
        uint32_t aid = pir_interp_alloc_new(it, sz, al, false, false, false, true);
        if (aid == 0u) return false;
        it->global_allocs[i] = aid;
    }

    /* Pass 2: populate global initializers and mark `data` segments readonly. */
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        uint32_t aid = it->global_allocs[i];
        if (aid == 0u) continue;
        PirPtr base = { aid, 0 };
        if (g->has_type && g->type.kind == PIR_TYPE_VECTOR && g->is_braced && !g->is_zeroinit) {
            PirScalarType elem = pir_vec_elem_scalar(g->type.u.vec);
            size_t esz = pir_scalar_bits(elem, it->ptr_bytes) / 8u;
            size_t total_sz = type_byte_size(g->type, it->ptr_bytes);
            size_t off = 0u;
            for (uint32_t k = 0u; k < g->item_count && off + esz <= total_sz; ++k) {
                const PirGlobalItem *gi = &g->items[k];
                PirPtr dst = { aid, (int64_t)off };
                if (gi->kind == PIR_GITEM_INT && esz <= 8u) {
                    if (elem == PIR_SCALAR_F32) {
                        float fv = gi->int_neg ? -(float)gi->int_lo : (float)gi->int_lo;
                        pir_interp_mem_write(it, dst, &fv, 4u, 1u);
                    } else if (elem == PIR_SCALAR_F64) {
                        double dv = gi->int_neg ? -(double)gi->int_lo : (double)gi->int_lo;
                        pir_interp_mem_write(it, dst, &dv, 8u, 1u);
                    } else {
                        uint64_t x = gi->int_neg ? (UINT64_C(0) - gi->int_lo) : gi->int_lo;
                        pir_interp_mem_write(it, dst, &x, esz, 1u);
                    }
                } else if (gi->kind == PIR_GITEM_FLOAT && esz <= 8u) {
                    if (elem == PIR_SCALAR_F32) {
                        uint32_t fb32 = (uint32_t)gi->float_bits;
                        pir_interp_mem_write(it, dst, &fb32, 4u, 1u);
                    } else {
                        uint64_t fb = gi->float_bits;
                        pir_interp_mem_write(it, dst, &fb, 8u, 1u);
                    }
                }
                off += esz;
            }
        } else if (g->has_type && g->type.kind == PIR_TYPE_SCALAR && !g->is_zeroinit && g->item_count == 1u) {
            const PirGlobalItem *gi = &g->items[0];
            size_t sz = type_byte_size(g->type, it->ptr_bytes);
            if (gi->kind == PIR_GITEM_INT) {
                uint64_t x = gi->int_neg ? (UINT64_C(0) - gi->int_lo) : gi->int_lo;
                if (g->type.u.scalar == PIR_SCALAR_PTR) {
                    pir_interp_mem_write_ptr(it, base, pir_interp_int_to_ptr(it, x), 1u);
                } else if (g->type.u.scalar == PIR_SCALAR_F32) {
                    float fv = gi->int_neg ? -(float)gi->int_lo : (float)gi->int_lo;
                    pir_interp_mem_write(it, base, &fv, 4u, 1u);
                } else if (g->type.u.scalar == PIR_SCALAR_F64) {
                    double dv = gi->int_neg ? -(double)gi->int_lo : (double)gi->int_lo;
                    pir_interp_mem_write(it, base, &dv, 8u, 1u);
                } else if (sz <= sizeof(x)) {
                    pir_interp_mem_write(it, base, &x, sz, 1u);
                } else {
                    pir_interp_fail(it, PIR_INTERP_ERR, "invalid scalar integer global size %zu", sz);
                    return false;
                }
            } else if (gi->kind == PIR_GITEM_FLOAT) {
                if (sz == 4u) {
                    uint32_t fb32 = (uint32_t)gi->float_bits;
                    pir_interp_mem_write(it, base, &fb32, 4u, 1u);
                } else if (sz <= sizeof(gi->float_bits)) {
                    pir_interp_mem_write(it, base, &gi->float_bits, sz, 1u);
                } else {
                    pir_interp_fail(it, PIR_INTERP_ERR, "invalid scalar float global size %zu", sz);
                    return false;
                }
            } else if (gi->kind == PIR_GITEM_RELOC) {
                PirOperand tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.kind = PIR_OPER_SYM;
                tmp.type = pir_type_scalar(PIR_SCALAR_PTR);
                tmp.u.sym.name   = gi->sym_name;
                tmp.u.sym.offset = gi->sym_offset;
                CallFrame dummy;
                memset(&dummy, 0, sizeof(dummy));
                PirVal rv = eval_operand(it, &dummy, &tmp);
                pir_interp_mem_write_ptr(it, base, rv.ptr, 1u);
            }
        } else if (!g->is_zeroinit) {
            size_t off = 0u;
            for (uint32_t k = 0u; k < g->item_count; ++k) {
                const PirGlobalItem *gi = &g->items[k];
                PirPtr dst = { aid, (int64_t)off };
                switch (gi->kind) {
                    case PIR_GITEM_BYTES:
                        if (gi->len > 0u) {
                            pir_interp_mem_write(it, dst, gi->bytes, (size_t)gi->len, 1u);
                            off += (size_t)gi->len;
                        }
                        break;
                    case PIR_GITEM_ZERO:
                        off += (size_t)(gi->len ? gi->len : 1u);
                        break;
                    case PIR_GITEM_INT: {
                        uint8_t b = (uint8_t)(gi->int_neg ? (UINT64_C(0) - gi->int_lo) : gi->int_lo);
                        pir_interp_mem_write(it, dst, &b, 1u, 1u);
                        off += 1u;
                        break;
                    }
                    case PIR_GITEM_FLOAT:
                        pir_interp_mem_write(it, dst, &gi->float_bits, 8u, 1u);
                        off += 8u;
                        break;
                    case PIR_GITEM_RELOC: {
                        PirOperand tmp;
                        memset(&tmp, 0, sizeof(tmp));
                        tmp.kind = PIR_OPER_SYM;
                        tmp.type = pir_type_scalar(PIR_SCALAR_PTR);
                        tmp.u.sym.name   = gi->sym_name;
                        tmp.u.sym.offset = gi->sym_offset;
                        CallFrame dummy;
                        memset(&dummy, 0, sizeof(dummy));
                        PirVal rv = eval_operand(it, &dummy, &tmp);
                        pir_interp_mem_write_ptr(it, dst, rv.ptr, 1u);
                        off += it->ptr_bytes;
                        break;
                    }
                }
            }
        }
        if (g->seg == PIR_GSEG_RODATA) {
            it->allocs.data[aid].readonly = true;
        }
    }
    return it->status == PIR_INTERP_OK;
}

PirInterpStatus pir_interp_run(const PirModule *mod, const PirTargetSpec *target,
                               int *out_exit_code) {
    PirInterp it;
    memset(&it, 0, sizeof(it));
    it.mod       = mod;
    it.target    = target;
    it.ptr_bytes = target ? target->ptr_bytes : 8u;
    it.next_addr = 0x10000000ULL;
    it.status    = PIR_INTERP_OK;

    PirFuncId main_id = PIR_INVALID_ID;
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        if (strcmp(mod->funcs.data[i].name, "main") == 0 && mod->funcs.data[i].is_defined) {
            main_id = i;
            break;
        }
    }
    if (main_id == PIR_INVALID_ID) {
        fprintf(stderr, "pir-interp: error: no defined '@main' function\n");
        return PIR_INTERP_ERR;
    }

    if (init_globals(&it)) {
        PirVal ret_val;
        memset(&ret_val, 0, sizeof(ret_val));
        exec_function(&it, main_id, NULL, NULL, 0u, &ret_val);
        if (it.unwinding && it.status == PIR_INTERP_OK) {
            pir_interp_fail(&it, PIR_INTERP_TRAP, "unhandled exception escaped @main");
        }
        if (it.status == PIR_INTERP_OK && !it.exited) {
            const PirFunction *mfn = &mod->funcs.data[main_id];
            if (mfn->ret_type.kind == PIR_TYPE_SCALAR && mfn->ret_type.u.scalar != PIR_SCALAR_VOID) {
                it.exit_code = (int)(int32_t)pir_val_get_u64(&ret_val);
            } else {
                it.exit_code = 0;
            }
        }
    }

    if (it.status == PIR_INTERP_TRAP) {
        fprintf(stderr, "pir-interp: trap: %s\n", it.err_msg);
    } else if (it.status == PIR_INTERP_UB) {
        fprintf(stderr, "pir-interp: ub: %s\n", it.err_msg);
    } else if (it.status == PIR_INTERP_ERR) {
        fprintf(stderr, "pir-interp: error: %s\n", it.err_msg);
    }

    if (out_exit_code) {
        *out_exit_code = it.exit_code;
    }

    for (uint32_t i = 0u; i < it.allocs.len; ++i) {
        free(it.allocs.data[i].bytes);
        free(it.allocs.data[i].init_bits);
        PIR_VEC_FREE(&it.allocs.data[i].ptr_tags);
    }
    PIR_VEC_FREE(&it.allocs);
    free(it.global_allocs);
    free(it.func_allocs);
    return it.status;
}
