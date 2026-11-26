/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "interp/interp.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool read_cstr(PirInterp *it, PirPtr p, char **out_buf, size_t *out_len) {
    if (p.alloc_id == 0u || p.alloc_id >= it->allocs.len) {
        pir_interp_fail(it, PIR_INTERP_UB, "null or invalid pointer passed to C string function");
        return false;
    }
    const PirAlloc *a = &it->allocs.data[p.alloc_id];
    if (!a->alive) {
        pir_interp_fail(it, PIR_INTERP_UB, "use-after-free in C string read");
        return false;
    }
    if (a->is_func) {
        pir_interp_fail(it, PIR_INTERP_UB, "cannot read C string from function pointer");
        return false;
    }
    if (p.offset < 0 || (uint64_t)p.offset >= a->size) {
        pir_interp_fail(it, PIR_INTERP_UB, "out-of-bounds C string read");
        return false;
    }
    size_t start = (size_t)p.offset;
    size_t len = 0u;
    while (start + len < a->size) {
        if (a->init_bits[start + len] == 0u) {
            pir_interp_fail(it, PIR_INTERP_UB, "uninitialized byte in C string");
            return false;
        }
        if (a->bytes[start + len] == 0u) {
            char *buf = (char *)malloc(len + 1u);
            if (!buf) abort();
            if (len > 0u) memcpy(buf, a->bytes + start, len);
            buf[len] = '\0';
            *out_buf = buf;
            if (out_len) *out_len = len;
            return true;
        }
        len++;
    }
    pir_interp_fail(it, PIR_INTERP_UB, "unterminated C string (out-of-bounds)");
    return false;
}

static bool shim_printf(PirInterp *it, const PirVal *args, uint32_t arg_count, int *out_written) {
    if (arg_count < 1u) {
        pir_interp_fail(it, PIR_INTERP_UB, "printf requires format string argument");
        return false;
    }
    char *fmt = NULL;
    if (!read_cstr(it, args[0].ptr, &fmt, NULL)) {
        return false;
    }
    int total = 0;
    uint32_t ai = 1u;
    for (const char *s = fmt; *s != '\0'; ++s) {
        if (*s != '%') {
            putchar((unsigned char)*s);
            total++;
            continue;
        }
        s++;
        if (*s == '\0') break;
        if (*s == '%') {
            putchar('%');
            total++;
            continue;
        }
        /* Capture optional flags/width/precision/length into small format buffer. */
        char fbuf[32];
        size_t fl = 0u;
        fbuf[fl++] = '%';
        while (*s && strchr("-+ #0", *s) != NULL) {
            if (fl >= sizeof(fbuf) - 4u) {
                free(fmt);
                pir_interp_fail(it, PIR_INTERP_UB, "printf format specifier too long");
                return false;
            }
            fbuf[fl++] = *s++;
        }
        while (*s >= '0' && *s <= '9') {
            if (fl >= sizeof(fbuf) - 4u) {
                free(fmt);
                pir_interp_fail(it, PIR_INTERP_UB, "printf format specifier too long");
                return false;
            }
            fbuf[fl++] = *s++;
        }
        if (*s == '.') {
            if (fl >= sizeof(fbuf) - 4u) {
                free(fmt);
                pir_interp_fail(it, PIR_INTERP_UB, "printf format specifier too long");
                return false;
            }
            fbuf[fl++] = *s++;
            while (*s >= '0' && *s <= '9') {
                if (fl >= sizeof(fbuf) - 4u) {
                    free(fmt);
                    pir_interp_fail(it, PIR_INTERP_UB, "printf format specifier too long");
                    return false;
                }
                fbuf[fl++] = *s++;
            }
        }
        int long_cnt = 0;
        while (*s == 'l' && long_cnt < 2) {
            long_cnt++;
            s++;
        }
        char spec = *s;
        if (spec == '\0') break;
        if (ai >= arg_count) {
            free(fmt);
            pir_interp_fail(it, PIR_INTERP_UB, "printf missing argument for '%%%c'", spec);
            return false;
        }
        const PirVal *av = &args[ai++];
        switch (spec) {
            case 'd': case 'i': {
                int64_t sv = 0;
                uint64_t raw = pir_val_get_u64(av);
                if (av->ty.kind == PIR_TYPE_SCALAR && av->ty.u.scalar == PIR_SCALAR_I64) {
                    sv = (int64_t)raw;
                } else if (av->ty.kind == PIR_TYPE_SCALAR && av->ty.u.scalar == PIR_SCALAR_I16) {
                    sv = (int64_t)(int16_t)raw;
                } else if (av->ty.kind == PIR_TYPE_SCALAR && av->ty.u.scalar == PIR_SCALAR_I8) {
                    sv = (int64_t)(int8_t)raw;
                } else {
                    sv = (int64_t)(int32_t)raw;
                }
                fbuf[fl++] = 'l'; fbuf[fl++] = 'l'; fbuf[fl++] = 'd'; fbuf[fl] = '\0';
                total += printf(fbuf, (long long)sv);
                break;
            }
            case 'u': case 'x': case 'X': {
                uint64_t uv = pir_val_get_u64(av);
                if (long_cnt == 0 && av->ty.kind == PIR_TYPE_SCALAR &&
                    av->ty.u.scalar == PIR_SCALAR_I32) {
                    uv = (uint32_t)uv;
                }
                fbuf[fl++] = 'l'; fbuf[fl++] = 'l'; fbuf[fl++] = spec; fbuf[fl] = '\0';
                total += printf(fbuf, (unsigned long long)uv);
                break;
            }
            case 'c': {
                int ch = (int)(uint8_t)pir_val_get_u64(av);
                putchar(ch);
                total++;
                break;
            }
            case 's': {
                char *str = NULL;
                if (!read_cstr(it, av->ptr, &str, NULL)) {
                    free(fmt);
                    return false;
                }
                fbuf[fl++] = 's'; fbuf[fl] = '\0';
                total += printf(fbuf, str);
                free(str);
                break;
            }
            case 'p': {
                uint64_t addr = pir_interp_ptr_to_int(it, av->ptr);
                total += printf("0x%" PRIx64, addr);
                break;
            }
            case 'f': case 'g': case 'e': {
                double dv = 0.0;
                if (av->ty.kind == PIR_TYPE_SCALAR && av->ty.u.scalar == PIR_SCALAR_F32) {
                    float fv = 0.0f;
                    memcpy(&fv, av->bytes, sizeof(fv));
                    dv = (double)fv;
                } else {
                    memcpy(&dv, av->bytes, sizeof(dv));
                }
                fbuf[fl++] = spec; fbuf[fl] = '\0';
                total += printf(fbuf, dv);
                break;
            }
            default:
                free(fmt);
                pir_interp_fail(it, PIR_INTERP_ERR, "unsupported printf specifier '%%%c'", spec);
                return false;
        }
    }
    fflush(stdout);
    free(fmt);
    *out_written = total;
    return true;
}

/* Portable unsigned 128-bit division/remainder (binary long division). */
static void u128_divmod(uint64_t a_lo, uint64_t a_hi, uint64_t b_lo, uint64_t b_hi,
                        uint64_t *q_lo, uint64_t *q_hi, uint64_t *r_lo, uint64_t *r_hi) {
    uint64_t ql = 0u, qh = 0u, rl = 0u, rh = 0u;
    for (int bit = 127; bit >= 0; --bit) {
        /* r <<= 1 */
        rh = (rh << 1u) | (rl >> 63u);
        rl <<= 1u;
        uint64_t abit = (bit >= 64) ? ((a_hi >> (unsigned)(bit - 64)) & 1u)
                                    : ((a_lo >> (unsigned)bit) & 1u);
        rl |= abit;
        /* if r >= b */
        if (rh > b_hi || (rh == b_hi && rl >= b_lo)) {
            uint64_t borrow = (rl < b_lo) ? 1u : 0u;
            rl -= b_lo;
            rh = rh - b_hi - borrow;
            if (bit >= 64) qh |= (uint64_t)1u << (unsigned)(bit - 64);
            else           ql |= (uint64_t)1u << (unsigned)bit;
        }
    }
    *q_lo = ql; *q_hi = qh;
    *r_lo = rl; *r_hi = rh;
}

static void i128_neg(uint64_t *lo, uint64_t *hi) {
    uint64_t nlo = ~(*lo) + 1u;
    uint64_t carry = (nlo == 0u) ? 1u : 0u;
    *lo = nlo;
    *hi = ~(*hi) + carry;
}

/* 128-bit div/rem helper shim: expects (out_ptr, a_lo, a_hi, b_lo, b_hi) writing 16B result. */
static bool shim_ti3(PirInterp *it, const char *name, const PirVal *args, uint32_t arg_count) {
    if (arg_count != 5u) {
        pir_interp_fail(it, PIR_INTERP_ERR, "%s expects 5 args (out_ptr, a_lo, a_hi, b_lo, b_hi)", name);
        return false;
    }
    PirPtr out_p  = args[0].ptr;
    uint64_t a_lo = pir_val_get_u64(&args[1]);
    uint64_t a_hi = pir_val_get_u64(&args[2]);
    uint64_t b_lo = pir_val_get_u64(&args[3]);
    uint64_t b_hi = pir_val_get_u64(&args[4]);
    if (b_lo == 0u && b_hi == 0u) {
        pir_interp_fail(it, PIR_INTERP_TRAP, "128-bit division by zero in %s", name);
        return false;
    }
    bool is_signed = (strcmp(name, "__divti3") == 0 || strcmp(name, "__modti3") == 0);
    bool want_rem  = (strcmp(name, "__modti3") == 0 || strcmp(name, "__umodti3") == 0);
    bool neg_a = false, neg_b = false;
    if (is_signed) {
        if ((a_hi >> 63u) != 0u) { neg_a = true; i128_neg(&a_lo, &a_hi); }
        if ((b_hi >> 63u) != 0u) { neg_b = true; i128_neg(&b_lo, &b_hi); }
    }
    uint64_t ql = 0u, qh = 0u, rl = 0u, rh = 0u;
    u128_divmod(a_lo, a_hi, b_lo, b_hi, &ql, &qh, &rl, &rh);
    uint64_t res_lo = want_rem ? rl : ql;
    uint64_t res_hi = want_rem ? rh : qh;
    if (is_signed && (want_rem ? neg_a : (neg_a != neg_b))) {
        i128_neg(&res_lo, &res_hi);
    }
    uint8_t buf[16];
    memcpy(buf, &res_lo, 8u);
    memcpy(buf + 8u, &res_hi, 8u);
    return pir_interp_mem_write(it, out_p, buf, 16u, 8u);
}

bool pir_interp_try_shim(PirInterp *it, const char *name,
                         const PirVal *args, uint32_t arg_count,
                         PirType ret_ty, PirVal *out_ret) {
    memset(out_ret, 0, sizeof(*out_ret));
    out_ret->ty = ret_ty;

    if (strcmp(name, "malloc") == 0) {
        if (arg_count != 1u) { pir_interp_fail(it, PIR_INTERP_UB, "malloc arity"); return true; }
        uint64_t sz = pir_val_get_u64(&args[0]);
        if (sz > (UINT64_C(1) << 30)) {
            out_ret->ptr.alloc_id = 0u;
            out_ret->ptr.offset   = 0;
            return true;
        }
        uint32_t id = pir_interp_alloc_new(it, (size_t)sz, 16u, false, true, false, false);
        out_ret->ptr.alloc_id = id;
        out_ret->ptr.offset   = 0;
        return true;
    }
    if (strcmp(name, "calloc") == 0) {
        if (arg_count != 2u) { pir_interp_fail(it, PIR_INTERP_UB, "calloc arity"); return true; }
        uint64_t n = pir_val_get_u64(&args[0]);
        uint64_t s = pir_val_get_u64(&args[1]);
        if (n != 0u && s > (UINT64_C(1) << 30) / n) {
            out_ret->ptr.alloc_id = 0u;
            out_ret->ptr.offset   = 0;
            return true;
        }
        size_t total = (size_t)(n * s);
        uint32_t id = pir_interp_alloc_new(it, total, 16u, false, true, false, true);
        out_ret->ptr.alloc_id = id;
        out_ret->ptr.offset   = 0;
        return true;
    }
    if (strcmp(name, "free") == 0) {
        if (arg_count != 1u) { pir_interp_fail(it, PIR_INTERP_UB, "free arity"); return true; }
        PirPtr p = args[0].ptr;
        if (p.alloc_id == 0u) return true; /* free(NULL) is a no-op */
        if (p.alloc_id >= it->allocs.len) {
            pir_interp_fail(it, PIR_INTERP_UB, "free of invalid pointer");
            return true;
        }
        PirAlloc *a = &it->allocs.data[p.alloc_id];
        if (!a->is_heap || p.offset != 0) {
            pir_interp_fail(it, PIR_INTERP_UB, "free of non-heap or interior pointer");
            return true;
        }
        if (!a->alive) {
            pir_interp_fail(it, PIR_INTERP_UB, "double free detected");
            return true;
        }
        a->alive = false;
        return true;
    }
    if (strcmp(name, "realloc") == 0) {
        if (arg_count != 2u) { pir_interp_fail(it, PIR_INTERP_UB, "realloc arity"); return true; }
        PirPtr p = args[0].ptr;
        uint64_t new_sz_u64 = pir_val_get_u64(&args[1]);
        if (new_sz_u64 > (UINT64_C(1) << 30)) {
            out_ret->ptr.alloc_id = 0u;
            out_ret->ptr.offset   = 0;
            return true;
        }
        size_t new_sz = (size_t)new_sz_u64;
        if (p.alloc_id == 0u) {
            uint32_t id = pir_interp_alloc_new(it, new_sz, 16u, false, true, false, false);
            out_ret->ptr.alloc_id = id;
            out_ret->ptr.offset   = 0;
            return true;
        }
        if (p.alloc_id >= it->allocs.len) {
            pir_interp_fail(it, PIR_INTERP_UB, "realloc of invalid pointer");
            return true;
        }
        PirAlloc *old_a = &it->allocs.data[p.alloc_id];
        if (!old_a->is_heap || !old_a->alive || p.offset != 0) {
            pir_interp_fail(it, PIR_INTERP_UB, "realloc of dead/non-heap/interior pointer");
            return true;
        }
        size_t old_sz = old_a->size;
        uint32_t new_id = pir_interp_alloc_new(it, new_sz, 16u, false, true, false, false);
        if (new_id == 0u) {
            out_ret->ptr.alloc_id = 0u;
            out_ret->ptr.offset   = 0;
            return true;
        }
        size_t copy_sz = old_sz < new_sz ? old_sz : new_sz;
        PirPtr np = { new_id, 0 };
        if (copy_sz > 0u) {
            pir_interp_mem_copy(it, np, p, copy_sz, false);
        }
        it->allocs.data[p.alloc_id].alive = false;
        out_ret->ptr = np;
        return true;
    }
    if (strcmp(name, "memcpy") == 0 || strcmp(name, "memmove") == 0) {
        if (arg_count != 3u) { pir_interp_fail(it, PIR_INTERP_UB, "memcpy/memmove arity"); return true; }
        size_t len = (size_t)pir_val_get_u64(&args[2]);
        pir_interp_mem_copy(it, args[0].ptr, args[1].ptr, len, strcmp(name, "memmove") == 0);
        out_ret->ptr = args[0].ptr;
        return true;
    }
    if (strcmp(name, "memset") == 0) {
        if (arg_count != 3u) { pir_interp_fail(it, PIR_INTERP_UB, "memset arity"); return true; }
        PirPtr dst = args[0].ptr;
        uint8_t byte = (uint8_t)pir_val_get_u64(&args[1]);
        size_t len = (size_t)pir_val_get_u64(&args[2]);
        if (len > 0u && pir_interp_check_access(it, dst, len, 1u, true, false)) {
            PirAlloc *a = &it->allocs.data[dst.alloc_id];
            size_t off = (size_t)dst.offset;
            memset(a->bytes + off, byte, len);
            memset(a->init_bits + off, 1, len);
            /* Invalidate overwritten ptr_tags. */
            for (uint32_t i = 0u; i < a->ptr_tags.len; ) {
                size_t to = a->ptr_tags.data[i].byte_off;
                if (to + it->ptr_bytes > off && to < off + len) {
                    a->ptr_tags.data[i] = a->ptr_tags.data[--a->ptr_tags.len];
                } else {
                    i++;
                }
            }
        }
        out_ret->ptr = dst;
        return true;
    }
    if (strcmp(name, "memcmp") == 0) {
        if (arg_count != 3u) { pir_interp_fail(it, PIR_INTERP_UB, "memcmp arity"); return true; }
        size_t len = (size_t)pir_val_get_u64(&args[2]);
        int res = 0;
        if (len > 0u &&
            pir_interp_check_access(it, args[0].ptr, len, 1u, false, true) &&
            pir_interp_check_access(it, args[1].ptr, len, 1u, false, true)) {
            const uint8_t *b1 = it->allocs.data[args[0].ptr.alloc_id].bytes + args[0].ptr.offset;
            const uint8_t *b2 = it->allocs.data[args[1].ptr.alloc_id].bytes + args[1].ptr.offset;
            res = memcmp(b1, b2, len);
        }
        pir_val_set_u64(out_ret, ret_ty, (uint64_t)(int64_t)res);
        return true;
    }
    if (strcmp(name, "strlen") == 0) {
        if (arg_count != 1u) { pir_interp_fail(it, PIR_INTERP_UB, "strlen arity"); return true; }
        char *s = NULL;
        size_t len = 0u;
        if (read_cstr(it, args[0].ptr, &s, &len)) {
            free(s);
            pir_val_set_u64(out_ret, ret_ty, (uint64_t)len);
        }
        return true;
    }
    if (strcmp(name, "putchar") == 0) {
        if (arg_count != 1u) { pir_interp_fail(it, PIR_INTERP_UB, "putchar arity"); return true; }
        int c = (int)(uint8_t)pir_val_get_u64(&args[0]);
        int r = putchar(c);
        fflush(stdout);
        pir_val_set_u64(out_ret, ret_ty, (uint64_t)(int64_t)r);
        return true;
    }
    if (strcmp(name, "puts") == 0) {
        if (arg_count != 1u) { pir_interp_fail(it, PIR_INTERP_UB, "puts arity"); return true; }
        char *s = NULL;
        if (read_cstr(it, args[0].ptr, &s, NULL)) {
            int r = puts(s);
            fflush(stdout);
            free(s);
            pir_val_set_u64(out_ret, ret_ty, (uint64_t)(int64_t)r);
        }
        return true;
    }
    if (strcmp(name, "printf") == 0) {
        int written = 0;
        if (shim_printf(it, args, arg_count, &written)) {
            pir_val_set_u64(out_ret, ret_ty, (uint64_t)(int64_t)written);
        }
        return true;
    }
    if (strcmp(name, "abort") == 0) {
        pir_interp_fail(it, PIR_INTERP_TRAP, "abort() called");
        return true;
    }
    if (strcmp(name, "exit") == 0) {
        int code = (arg_count >= 1u) ? (int)(int32_t)pir_val_get_u64(&args[0]) : 0;
        it->exited    = true;
        it->exit_code = code;
        return true;
    }
    if (strcmp(name, "__pir_setjmp") == 0) {
        /* Handled directly inside exec_function to capture frame checkpoint. */
        pir_val_set_u64(out_ret, ret_ty, 0u);
        return true;
    }
    if (strcmp(name, "__pir_unwind_resume") == 0) {
        if (arg_count != 1u) {
            pir_interp_fail(it, PIR_INTERP_UB, "__pir_unwind_resume arity");
            return true;
        }
        PirPtr exn = args[0].ptr;
        /* Locate TLS global @__pir_exc_head and read active frame record. */
        uint32_t head_alloc = 0u;
        for (uint32_t i = 0u; i < it->mod->globals.len; ++i) {
            if (strcmp(it->mod->globals.data[i].name, "__pir_exc_head") == 0) {
                head_alloc = it->global_allocs[i];
                break;
            }
        }
        if (head_alloc == 0u) {
            pir_interp_fail(it, PIR_INTERP_TRAP, "unhandled exception (no @__pir_exc_head)");
            return true;
        }
        PirPtr head_p = { head_alloc, 0 };
        PirPtr frame_p = { 0u, 0 };
        if (!pir_interp_mem_read_ptr(it, head_p, &frame_p, it->ptr_bytes)) {
            return true;
        }
        if (frame_p.alloc_id == 0u) {
            pir_interp_fail(it, PIR_INTERP_TRAP, "unhandled exception (empty shadow stack)");
            return true;
        }
        /* Store exception payload at frame.exn (offset = ptr_bytes). */
        PirPtr exn_slot = { frame_p.alloc_id, frame_p.offset + (int64_t)it->ptr_bytes };
        if (!pir_interp_mem_write_ptr(it, exn_slot, exn, it->ptr_bytes)) {
            return true;
        }
        it->unwinding                 = true;
        it->unwind_target_jb.alloc_id = frame_p.alloc_id;
        it->unwind_target_jb.offset   = frame_p.offset + (int64_t)(2u * it->ptr_bytes);
        it->unwind_exn                = exn;
        return true;
    }
    if (strcmp(name, "__divti3") == 0 || strcmp(name, "__udivti3") == 0 ||
        strcmp(name, "__modti3") == 0 || strcmp(name, "__umodti3") == 0) {
        shim_ti3(it, name, args, arg_count);
        return true;
    }
    return false;
}
