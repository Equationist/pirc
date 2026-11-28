/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "interp/interp.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t shim_pair_u32(const PirInterp *it, const PirVal *a, const PirVal *b);

static bool read_cstr_n(PirInterp *it, PirPtr p, bool has_max, size_t max_n,
                        char **out_buf, size_t *out_len) {
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
    if (has_max && max_n == 0u) {
        if (p.offset < 0 || (uint64_t)p.offset > a->size) {
            pir_interp_fail(it, PIR_INTERP_UB, "out-of-bounds C string read");
            return false;
        }
        char *buf = (char *)malloc(1u);
        if (!buf) abort();
        buf[0] = '\0';
        *out_buf = buf;
        if (out_len) *out_len = 0u;
        return true;
    }
    if (p.offset < 0 || (uint64_t)p.offset >= a->size) {
        pir_interp_fail(it, PIR_INTERP_UB, "out-of-bounds C string read");
        return false;
    }
    size_t start = (size_t)p.offset;
    size_t len = 0u;
    while (start + len < a->size) {
        if (has_max && len >= max_n) {
            char *buf = (char *)malloc(len + 1u);
            if (!buf) abort();
            if (len > 0u) memcpy(buf, a->bytes + start, len);
            buf[len] = '\0';
            *out_buf = buf;
            if (out_len) *out_len = len;
            return true;
        }
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
    if (has_max && len == max_n) {
        char *buf = (char *)malloc(len + 1u);
        if (!buf) abort();
        if (len > 0u) memcpy(buf, a->bytes + start, len);
        buf[len] = '\0';
        *out_buf = buf;
        if (out_len) *out_len = len;
        return true;
    }
    pir_interp_fail(it, PIR_INTERP_UB, "unterminated C string (out-of-bounds)");
    return false;
}

static bool read_cstr(PirInterp *it, PirPtr p, char **out_buf, size_t *out_len) {
    return read_cstr_n(it, p, false, 0u, out_buf, out_len);
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
        bool flag_minus = false, flag_plus = false, flag_space = false;
        bool flag_hash = false, flag_zero = false;
        while (*s) {
            if (*s == '-')      { flag_minus = true; s++; }
            else if (*s == '+') { flag_plus  = true; s++; }
            else if (*s == ' ') { flag_space = true; s++; }
            else if (*s == '#') { flag_hash  = true; s++; }
            else if (*s == '0') { flag_zero  = true; s++; }
            else break;
        }
        bool has_width = false;
        uint32_t width_val = 0u;
        if (*s == '*') {
            s++;
            if (ai >= arg_count) {
                free(fmt);
                pir_interp_fail(it, PIR_INTERP_UB, "printf missing dynamic width argument");
                return false;
            }
            int32_t wv = 0;
            if (it->target && it->target->caps.max_int_bits == 16u &&
                ai + 1u < arg_count &&
                args[ai].ty.kind == PIR_TYPE_SCALAR && args[ai].ty.u.scalar == PIR_SCALAR_I16 &&
                args[ai + 1u].ty.kind == PIR_TYPE_SCALAR && args[ai + 1u].ty.u.scalar == PIR_SCALAR_I16) {
                wv = (int32_t)shim_pair_u32(it, &args[ai], &args[ai + 1u]);
                ai += 2u;
            } else {
                wv = (int32_t)pir_val_get_u64(&args[ai++]);
            }
            if (wv < 0) {
                flag_minus = true;
                uint64_t mag = (uint64_t)(-(int64_t)wv);
                if (mag > 65536u) {
                    free(fmt);
                    pir_interp_fail(it, PIR_INTERP_UB, "printf format width too large");
                    return false;
                }
                width_val = (uint32_t)mag;
            } else {
                if ((uint32_t)wv > 65536u) {
                    free(fmt);
                    pir_interp_fail(it, PIR_INTERP_UB, "printf format width too large");
                    return false;
                }
                width_val = (uint32_t)wv;
            }
            has_width = true;
        } else {
            while (*s >= '0' && *s <= '9') {
                has_width = true;
                width_val = width_val * 10u + (uint32_t)(*s - '0');
                if (width_val > 65536u) {
                    free(fmt);
                    pir_interp_fail(it, PIR_INTERP_UB, "printf format width too large");
                    return false;
                }
                s++;
            }
        }
        bool has_prec = false;
        uint32_t prec_val = 0u;
        if (*s == '.') {
            s++;
            if (*s == '*') {
                s++;
                if (ai >= arg_count) {
                    free(fmt);
                    pir_interp_fail(it, PIR_INTERP_UB, "printf missing dynamic precision argument");
                    return false;
                }
                int32_t pv = 0;
                if (it->target && it->target->caps.max_int_bits == 16u &&
                    ai + 1u < arg_count &&
                    args[ai].ty.kind == PIR_TYPE_SCALAR && args[ai].ty.u.scalar == PIR_SCALAR_I16 &&
                    args[ai + 1u].ty.kind == PIR_TYPE_SCALAR && args[ai + 1u].ty.u.scalar == PIR_SCALAR_I16) {
                    pv = (int32_t)shim_pair_u32(it, &args[ai], &args[ai + 1u]);
                    ai += 2u;
                } else {
                    pv = (int32_t)pir_val_get_u64(&args[ai++]);
                }
                if (pv >= 0) {
                    if ((uint32_t)pv > 65536u) {
                        free(fmt);
                        pir_interp_fail(it, PIR_INTERP_UB, "printf format precision too large");
                        return false;
                    }
                    has_prec = true;
                    prec_val = (uint32_t)pv;
                }
            } else {
                has_prec = true;
                while (*s >= '0' && *s <= '9') {
                    prec_val = prec_val * 10u + (uint32_t)(*s - '0');
                    if (prec_val > 65536u) {
                        free(fmt);
                        pir_interp_fail(it, PIR_INTERP_UB, "printf format precision too large");
                        return false;
                    }
                    s++;
                }
            }
        }
        enum { LEN_NONE = 0, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z, LEN_T, LEN_J } lmod = LEN_NONE;
        if (*s == 'h') {
            s++;
            if (*s == 'h') { lmod = LEN_HH; s++; }
            else lmod = LEN_H;
        } else if (*s == 'l') {
            s++;
            if (*s == 'l') { lmod = LEN_LL; s++; }
            else lmod = LEN_L;
        } else if (*s == 'z') {
            lmod = LEN_Z; s++;
        } else if (*s == 't') {
            lmod = LEN_T; s++;
        } else if (*s == 'j') {
            lmod = LEN_J; s++;
        }
        char spec = *s;
        if (spec == '\0') break;
        if (ai >= arg_count) {
            free(fmt);
            pir_interp_fail(it, PIR_INTERP_UB, "printf missing argument for '%%%c'", spec);
            return false;
        }
        const PirVal *av = &args[ai++];
        bool long_is_64 = (it->ptr_bytes >= 8u && (!it->target || it->target->os != PIR_OS_WINDOWS));
        bool is_64 = (lmod == LEN_LL || lmod == LEN_J ||
                      (lmod == LEN_L && long_is_64) ||
                      ((lmod == LEN_Z || lmod == LEN_T) && it->ptr_bytes >= 8u));
        bool is_32_on_16 = (!is_64 && lmod != LEN_HH && lmod != LEN_H &&
                            !(it->ptr_bytes == 2u && (lmod == LEN_Z || lmod == LEN_T)) &&
                            it->target && it->target->caps.max_int_bits == 16u &&
                            av->ty.kind == PIR_TYPE_SCALAR && av->ty.u.scalar == PIR_SCALAR_I16 &&
                            ai < arg_count &&
                            args[ai].ty.kind == PIR_TYPE_SCALAR && args[ai].ty.u.scalar == PIR_SCALAR_I16);
        bool split_64_32 = (is_64 && it->target && it->target->caps.max_int_bits == 32u &&
                            av->ty.kind == PIR_TYPE_SCALAR && av->ty.u.scalar == PIR_SCALAR_I32 &&
                            ai < arg_count &&
                            args[ai].ty.kind == PIR_TYPE_SCALAR && args[ai].ty.u.scalar == PIR_SCALAR_I32);
        bool split_64_16 = (is_64 && it->target && it->target->caps.max_int_bits == 16u &&
                            av->ty.kind == PIR_TYPE_SCALAR && av->ty.u.scalar == PIR_SCALAR_I16 &&
                            ai + 2u < arg_count);
        bool split_val = false;
        uint64_t split_64_val = 0u;
        if (split_64_32) {
            uint64_t u0 = (uint32_t)pir_val_get_u64(av);
            uint64_t u1 = (uint32_t)pir_val_get_u64(&args[ai++]);
            split_64_val = (it->target && it->target->is_big_endian)
                           ? ((u0 << 32u) | u1) : (u0 | (u1 << 32u));
            split_val = true;
        } else if (split_64_16) {
            uint64_t w0 = (uint16_t)pir_val_get_u64(av);
            uint64_t w1 = (uint16_t)pir_val_get_u64(&args[ai++]);
            uint64_t w2 = (uint16_t)pir_val_get_u64(&args[ai++]);
            uint64_t w3 = (uint16_t)pir_val_get_u64(&args[ai++]);
            split_64_val = (it->target && it->target->is_big_endian)
                           ? ((w0 << 48u) | (w1 << 32u) | (w2 << 16u) | w3)
                           : (w0 | (w1 << 16u) | (w2 << 32u) | (w3 << 48u));
            split_val = true;
        } else if (is_32_on_16 && (spec == 'd' || spec == 'i' || spec == 'u' ||
                                   spec == 'o' || spec == 'x' || spec == 'X' || spec == 'c')) {
            uint32_t w0 = (uint16_t)pir_val_get_u64(av);
            uint32_t w1 = (uint16_t)pir_val_get_u64(&args[ai++]);
            split_64_val = (uint64_t)((it->target && it->target->is_big_endian)
                                      ? ((w0 << 16u) | w1) : (w0 | (w1 << 16u)));
            split_val = true;
        }
        char fbuf[48];
        size_t fl = 0u;
        fbuf[fl++] = '%';
        switch (spec) {
            case 'd': case 'i': {
                int64_t sv = 0;
                uint64_t raw = split_val ? split_64_val : pir_val_get_u64(av);
                if (lmod == LEN_HH)     sv = (int64_t)(int8_t)raw;
                else if (lmod == LEN_H) sv = (int64_t)(int16_t)raw;
                else if (is_64)         sv = (int64_t)raw;
                else                    sv = (int64_t)(int32_t)raw;
                if (flag_minus) fbuf[fl++] = '-';
                if (flag_plus)  fbuf[fl++] = '+';
                else if (flag_space) fbuf[fl++] = ' ';
                if (flag_zero && !flag_minus && !has_prec) fbuf[fl++] = '0';
                if (has_width) fl += (size_t)snprintf(fbuf + fl, sizeof(fbuf) - fl, "%u", width_val);
                if (has_prec)  fl += (size_t)snprintf(fbuf + fl, sizeof(fbuf) - fl, ".%u", prec_val);
                snprintf(fbuf + fl, sizeof(fbuf) - fl, "lld");
                total += printf(fbuf, (long long)sv);
                break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                uint64_t uv = split_val ? split_64_val : pir_val_get_u64(av);
                if (lmod == LEN_HH)     uv = (uint8_t)uv;
                else if (lmod == LEN_H) uv = (uint16_t)uv;
                else if (!is_64)        uv = (uint32_t)uv;
                if (flag_minus) fbuf[fl++] = '-';
                if (flag_hash && spec != 'u') fbuf[fl++] = '#';
                if (flag_zero && !flag_minus && !has_prec) fbuf[fl++] = '0';
                if (has_width) fl += (size_t)snprintf(fbuf + fl, sizeof(fbuf) - fl, "%u", width_val);
                if (has_prec)  fl += (size_t)snprintf(fbuf + fl, sizeof(fbuf) - fl, ".%u", prec_val);
                snprintf(fbuf + fl, sizeof(fbuf) - fl, "ll%c", spec);
                total += printf(fbuf, (unsigned long long)uv);
                break;
            }
            case 'c': {
                int ch = (int)(unsigned char)(split_val ? split_64_val : pir_val_get_u64(av));
                if (flag_minus) fbuf[fl++] = '-';
                if (has_width) fl += (size_t)snprintf(fbuf + fl, sizeof(fbuf) - fl, "%u", width_val);
                fbuf[fl++] = 'c'; fbuf[fl] = '\0';
                total += printf(fbuf, ch);
                break;
            }
            case 's': {
                char *str = NULL;
                if (!read_cstr_n(it, av->ptr, has_prec, (size_t)prec_val, &str, NULL)) {
                    free(fmt);
                    return false;
                }
                if (flag_minus) fbuf[fl++] = '-';
                if (has_width) fl += (size_t)snprintf(fbuf + fl, sizeof(fbuf) - fl, "%u", width_val);
                if (has_prec)  fl += (size_t)snprintf(fbuf + fl, sizeof(fbuf) - fl, ".%u", prec_val);
                fbuf[fl++] = 's'; fbuf[fl] = '\0';
                total += printf(fbuf, str);
                free(str);
                break;
            }
            case 'p': {
                uint64_t addr = pir_interp_ptr_to_int(it, av->ptr);
                char pbuf[32];
                snprintf(pbuf, sizeof(pbuf), "0x%" PRIx64, addr);
                if (flag_minus) fbuf[fl++] = '-';
                if (has_width) fl += (size_t)snprintf(fbuf + fl, sizeof(fbuf) - fl, "%u", width_val);
                fbuf[fl++] = 's'; fbuf[fl] = '\0';
                total += printf(fbuf, pbuf);
                break;
            }
            case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': case 'a': case 'A': {
                double dv = 0.0;
                if (av->ty.kind == PIR_TYPE_SCALAR && av->ty.u.scalar == PIR_SCALAR_F32) {
                    float fv = 0.0f;
                    memcpy(&fv, av->bytes, sizeof(fv));
                    dv = (double)fv;
                } else if (av->ty.kind == PIR_TYPE_SCALAR && av->ty.u.scalar == PIR_SCALAR_I16 &&
                           it->target && it->target->caps.max_int_bits == 16u && ai + 2u < arg_count) {
                    uint64_t w0 = (uint16_t)pir_val_get_u64(av);
                    uint64_t w1 = (uint16_t)pir_val_get_u64(&args[ai++]);
                    uint64_t w2 = (uint16_t)pir_val_get_u64(&args[ai++]);
                    uint64_t w3 = (uint16_t)pir_val_get_u64(&args[ai++]);
                    uint64_t bits = (it->target && it->target->is_big_endian)
                                    ? ((w0 << 48u) | (w1 << 32u) | (w2 << 16u) | w3)
                                    : (w0 | (w1 << 16u) | (w2 << 32u) | (w3 << 48u));
                    memcpy(&dv, &bits, sizeof(dv));
                } else if (av->ty.kind == PIR_TYPE_SCALAR && av->ty.u.scalar == PIR_SCALAR_I32 &&
                           it->target && it->target->caps.max_int_bits == 32u && ai < arg_count) {
                    uint64_t u0 = (uint32_t)pir_val_get_u64(av);
                    uint64_t u1 = (uint32_t)pir_val_get_u64(&args[ai++]);
                    uint64_t bits = (it->target && it->target->is_big_endian)
                                    ? ((u0 << 32u) | u1) : (u0 | (u1 << 32u));
                    memcpy(&dv, &bits, sizeof(dv));
                } else {
                    memcpy(&dv, av->bytes, sizeof(dv));
                }
                if (flag_minus) fbuf[fl++] = '-';
                if (flag_plus)  fbuf[fl++] = '+';
                else if (flag_space) fbuf[fl++] = ' ';
                if (flag_hash)  fbuf[fl++] = '#';
                if (flag_zero && !flag_minus) fbuf[fl++] = '0';
                if (has_width) fl += (size_t)snprintf(fbuf + fl, sizeof(fbuf) - fl, "%u", width_val);
                if (has_prec)  fl += (size_t)snprintf(fbuf + fl, sizeof(fbuf) - fl, ".%u", prec_val);
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

static bool shim_is_be(const PirInterp *it) {
    return it->target && it->target->is_big_endian;
}

static uint32_t shim_max_int_bits(const PirInterp *it) {
    return (it->target && it->target->caps.max_int_bits > 0u) ? it->target->caps.max_int_bits : 64u;
}

static uint32_t shim_pair_u32(const PirInterp *it, const PirVal *a, const PirVal *b) {
    uint32_t u0 = (uint16_t)pir_val_get_u64(a);
    uint32_t u1 = (uint16_t)pir_val_get_u64(b);
    return shim_is_be(it) ? ((u0 << 16u) | u1) : (u0 | (u1 << 16u));
}

static uint64_t shim_pair_u64(const PirInterp *it, const PirVal *a, const PirVal *b) {
    uint64_t u0 = (uint32_t)pir_val_get_u64(a);
    uint64_t u1 = (uint32_t)pir_val_get_u64(b);
    return shim_is_be(it) ? ((u0 << 32u) | u1) : (u0 | (u1 << 32u));
}

static uint64_t shim_quad16_u64(const PirInterp *it, const PirVal *a) {
    uint64_t w0 = (uint16_t)pir_val_get_u64(&a[0]);
    uint64_t w1 = (uint16_t)pir_val_get_u64(&a[1]);
    uint64_t w2 = (uint16_t)pir_val_get_u64(&a[2]);
    uint64_t w3 = (uint16_t)pir_val_get_u64(&a[3]);
    if (shim_is_be(it)) {
        return (w0 << 48u) | (w1 << 32u) | (w2 << 16u) | w3;
    }
    return w0 | (w1 << 16u) | (w2 << 32u) | (w3 << 48u);
}

static void shim_encode_u32(const PirInterp *it, uint8_t *buf, uint32_t v) {
    if (shim_is_be(it)) {
        for (uint32_t i = 0u; i < 4u; ++i) {
            buf[i] = (uint8_t)((v >> ((3u - i) * 8u)) & 0xFFu);
        }
    } else {
        memcpy(buf, &v, 4u);
    }
}

static uint32_t shim_decode_u32(const PirInterp *it, const uint8_t *buf) {
    if (shim_is_be(it)) {
        uint32_t v = 0u;
        for (uint32_t i = 0u; i < 4u; ++i) {
            v = (v << 8u) | (uint32_t)buf[i];
        }
        return v;
    }
    uint32_t v = 0u;
    memcpy(&v, buf, 4u);
    return v;
}

static void shim_encode_u64(const PirInterp *it, uint8_t *buf, uint64_t v) {
    if (shim_is_be(it)) {
        for (uint32_t i = 0u; i < 8u; ++i) {
            buf[i] = (uint8_t)((v >> ((7u - i) * 8u)) & 0xFFu);
        }
    } else {
        memcpy(buf, &v, 8u);
    }
}

static uint64_t shim_decode_u64(const PirInterp *it, const uint8_t *buf) {
    if (shim_is_be(it)) {
        uint64_t v = 0u;
        for (uint32_t i = 0u; i < 8u; ++i) {
            v = (v << 8u) | (uint64_t)buf[i];
        }
        return v;
    }
    uint64_t v = 0u;
    memcpy(&v, buf, 8u);
    return v;
}

static bool shim_write_u32_ret(PirInterp *it, PirPtr out_p, uint32_t v) {
    uint8_t buf[4];
    shim_encode_u32(it, buf, v);
    return pir_interp_mem_write(it, out_p, buf, 4u, 1u);
}

static bool shim_write_u64_ret(PirInterp *it, PirPtr out_p, uint64_t v) {
    uint8_t buf[8];
    shim_encode_u64(it, buf, v);
    return pir_interp_mem_write(it, out_p, buf, 8u, 1u);
}

static bool shim_read_int_arg(const PirInterp *it, const PirVal *args, uint32_t arg_count,
                              uint32_t *idx, uint32_t bits, uint64_t *out_v) {
    uint32_t mib = shim_max_int_bits(it);
    if (bits <= mib) {
        if (*idx >= arg_count) return false;
        *out_v = pir_val_get_u64(&args[(*idx)++]);
        return true;
    }
    if (bits == 32u && mib == 16u) {
        if (*idx + 2u > arg_count) return false;
        *out_v = (uint64_t)shim_pair_u32(it, &args[*idx], &args[*idx + 1u]);
        *idx += 2u;
        return true;
    }
    if (bits == 64u && mib == 32u) {
        if (*idx + 2u > arg_count) return false;
        *out_v = shim_pair_u64(it, &args[*idx], &args[*idx + 1u]);
        *idx += 2u;
        return true;
    }
    if (bits == 64u && mib == 16u) {
        if (*idx + 4u > arg_count) return false;
        *out_v = shim_quad16_u64(it, &args[*idx]);
        *idx += 4u;
        return true;
    }
    return false;
}

static bool shim_is_void_ret(PirType t) {
    return t.kind == PIR_TYPE_SCALAR && t.u.scalar == PIR_SCALAR_VOID;
}

/* Soft-float helper shims (__addsf3 .. __pir_fcmp_df). */
static bool shim_try_soft_float(PirInterp *it, const char *name,
                                const PirVal *args, uint32_t arg_count,
                                PirType ret_ty, PirVal *out_ret) {
    if (strcmp(name, "__pir_fcmp_sf") == 0) {
        uint32_t idx = 0u;
        uint64_t ab = 0u, bb = 0u;
        if (!shim_read_int_arg(it, args, arg_count, &idx, 32u, &ab) ||
            !shim_read_int_arg(it, args, arg_count, &idx, 32u, &bb)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "__pir_fcmp_sf arity");
            return true;
        }
        uint32_t au = (uint32_t)ab, bu = (uint32_t)bb;
        float fa = 0.0f, fb = 0.0f;
        memcpy(&fa, &au, 4u); memcpy(&fb, &bu, 4u);
        uint8_t r = (isnan(fa) || isnan(fb)) ? 2u : (fa < fb ? (uint8_t)0xFFu : (fa > fb ? 1u : 0u));
        pir_val_set_u64(out_ret, ret_ty, r);
        return true;
    }
    if (strcmp(name, "__pir_fcmp_df") == 0) {
        uint32_t idx = 0u;
        uint64_t ab = 0u, bb = 0u;
        if (!shim_read_int_arg(it, args, arg_count, &idx, 64u, &ab) ||
            !shim_read_int_arg(it, args, arg_count, &idx, 64u, &bb)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "__pir_fcmp_df arity");
            return true;
        }
        double da = 0.0, db = 0.0;
        memcpy(&da, &ab, 8u); memcpy(&db, &bb, 8u);
        uint8_t r = (isnan(da) || isnan(db)) ? 2u : (da < db ? (uint8_t)0xFFu : (da > db ? 1u : 0u));
        pir_val_set_u64(out_ret, ret_ty, r);
        return true;
    }
    if (strcmp(name, "__addsf3") == 0 || strcmp(name, "__subsf3") == 0 ||
        strcmp(name, "__mulsf3") == 0 || strcmp(name, "__divsf3") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u, bb = 0u;
        if ((sret && arg_count < 1u) ||
            !shim_read_int_arg(it, args, arg_count, &idx, 32u, &ab) ||
            !shim_read_int_arg(it, args, arg_count, &idx, 32u, &bb)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "%s arity", name);
            return true;
        }
        uint32_t au = (uint32_t)ab, bu = (uint32_t)bb, ru = 0u;
        float fa = 0.0f, fb = 0.0f, fr = 0.0f;
        memcpy(&fa, &au, 4u); memcpy(&fb, &bu, 4u);
        if (name[2] == 'a')      fr = fa + fb;
        else if (name[2] == 's') fr = fa - fb;
        else if (name[2] == 'm') fr = fa * fb;
        else                     fr = fa / fb;
        memcpy(&ru, &fr, 4u);
        if (sret) shim_write_u32_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__adddf3") == 0 || strcmp(name, "__subdf3") == 0 ||
        strcmp(name, "__muldf3") == 0 || strcmp(name, "__divdf3") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u, bb = 0u, ru = 0u;
        if ((sret && arg_count < 1u) ||
            !shim_read_int_arg(it, args, arg_count, &idx, 64u, &ab) ||
            !shim_read_int_arg(it, args, arg_count, &idx, 64u, &bb)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "%s arity", name);
            return true;
        }
        double da = 0.0, db = 0.0, dr = 0.0;
        memcpy(&da, &ab, 8u); memcpy(&db, &bb, 8u);
        if (name[2] == 'a')      dr = da + db;
        else if (name[2] == 's') dr = da - db;
        else if (name[2] == 'm') dr = da * db;
        else                     dr = da / db;
        memcpy(&ru, &dr, 8u);
        if (sret) shim_write_u64_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__extendsfdf2") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u, ru = 0u;
        if ((sret && arg_count < 1u) || !shim_read_int_arg(it, args, arg_count, &idx, 32u, &ab)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "__extendsfdf2 arity");
            return true;
        }
        uint32_t au = (uint32_t)ab;
        float fa = 0.0f; memcpy(&fa, &au, 4u);
        double dr = (double)fa; memcpy(&ru, &dr, 8u);
        if (sret) shim_write_u64_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__truncdfsf2") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u;
        if ((sret && arg_count < 1u) || !shim_read_int_arg(it, args, arg_count, &idx, 64u, &ab)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "__truncdfsf2 arity");
            return true;
        }
        double da = 0.0; memcpy(&da, &ab, 8u);
        float fr = (float)da; uint32_t ru = 0u; memcpy(&ru, &fr, 4u);
        if (sret) shim_write_u32_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__floatsisf") == 0 || strcmp(name, "__floatunsisf") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u;
        if ((sret && arg_count < 1u) || !shim_read_int_arg(it, args, arg_count, &idx, 32u, &ab)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "%s arity", name);
            return true;
        }
        float fr = (name[7] == 'u') ? (float)(uint32_t)ab : (float)(int32_t)(uint32_t)ab;
        uint32_t ru = 0u; memcpy(&ru, &fr, 4u);
        if (sret) shim_write_u32_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__floatsidf") == 0 || strcmp(name, "__floatunsidf") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u, ru = 0u;
        if ((sret && arg_count < 1u) || !shim_read_int_arg(it, args, arg_count, &idx, 32u, &ab)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "%s arity", name);
            return true;
        }
        double dr = (name[7] == 'u') ? (double)(uint32_t)ab : (double)(int32_t)(uint32_t)ab;
        memcpy(&ru, &dr, 8u);
        if (sret) shim_write_u64_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__floatdisf") == 0 || strcmp(name, "__floatundisf") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u;
        if ((sret && arg_count < 1u) || !shim_read_int_arg(it, args, arg_count, &idx, 64u, &ab)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "%s arity", name);
            return true;
        }
        float fr = (name[7] == 'u') ? (float)ab : (float)(int64_t)ab;
        uint32_t ru = 0u; memcpy(&ru, &fr, 4u);
        if (sret) shim_write_u32_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__floatdidf") == 0 || strcmp(name, "__floatundidf") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u, ru = 0u;
        if ((sret && arg_count < 1u) || !shim_read_int_arg(it, args, arg_count, &idx, 64u, &ab)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "%s arity", name);
            return true;
        }
        double dr = (name[7] == 'u') ? (double)ab : (double)(int64_t)ab;
        memcpy(&ru, &dr, 8u);
        if (sret) shim_write_u64_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__fixsfsi") == 0 || strcmp(name, "__fixunssfsi") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u;
        if ((sret && arg_count < 1u) || !shim_read_int_arg(it, args, arg_count, &idx, 32u, &ab)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "%s arity", name);
            return true;
        }
        uint32_t au = (uint32_t)ab;
        float fa = 0.0f; memcpy(&fa, &au, 4u);
        uint32_t ru = 0u;
        if (name[5] == 'u') {
            ru = (isnan(fa) || fa < 0.0f) ? 0u : (fa >= 4294967296.0f ? UINT32_MAX : (uint32_t)fa);
        } else {
            ru = isnan(fa) ? 0u : (fa <= -2147483648.0f ? (uint32_t)INT32_MIN : (fa >= 2147483648.0f ? (uint32_t)INT32_MAX : (uint32_t)(int32_t)fa));
        }
        if (sret) shim_write_u32_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__fixdfsi") == 0 || strcmp(name, "__fixunsdfsi") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u;
        if ((sret && arg_count < 1u) || !shim_read_int_arg(it, args, arg_count, &idx, 64u, &ab)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "%s arity", name);
            return true;
        }
        double da = 0.0; memcpy(&da, &ab, 8u);
        uint32_t ru = 0u;
        if (name[5] == 'u') {
            ru = (isnan(da) || da < 0.0) ? 0u : (da >= 4294967296.0 ? UINT32_MAX : (uint32_t)da);
        } else {
            ru = isnan(da) ? 0u : (da <= -2147483648.0 ? (uint32_t)INT32_MIN : (da >= 2147483648.0 ? (uint32_t)INT32_MAX : (uint32_t)(int32_t)da));
        }
        if (sret) shim_write_u32_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__fixsfdi") == 0 || strcmp(name, "__fixunssfdi") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u;
        if ((sret && arg_count < 1u) || !shim_read_int_arg(it, args, arg_count, &idx, 32u, &ab)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "%s arity", name);
            return true;
        }
        uint32_t au = (uint32_t)ab;
        float fa = 0.0f; memcpy(&fa, &au, 4u);
        uint64_t ru = 0u;
        if (name[5] == 'u') {
            ru = (isnan(fa) || fa < 0.0f) ? 0u : (fa >= 18446744073709551616.0f ? UINT64_MAX : (uint64_t)fa);
        } else {
            ru = isnan(fa) ? 0u : (fa <= -9223372036854775808.0f ? (uint64_t)INT64_MIN : (fa >= 9223372036854775808.0f ? (uint64_t)INT64_MAX : (uint64_t)(int64_t)fa));
        }
        if (sret) shim_write_u64_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    if (strcmp(name, "__fixdfdi") == 0 || strcmp(name, "__fixunsdfdi") == 0) {
        bool sret = shim_is_void_ret(ret_ty);
        uint32_t idx = sret ? 1u : 0u;
        uint64_t ab = 0u;
        if ((sret && arg_count < 1u) || !shim_read_int_arg(it, args, arg_count, &idx, 64u, &ab)) {
            pir_interp_fail(it, PIR_INTERP_ERR, "%s arity", name);
            return true;
        }
        double da = 0.0; memcpy(&da, &ab, 8u);
        uint64_t ru = 0u;
        if (name[5] == 'u') {
            ru = (isnan(da) || da < 0.0) ? 0u : (da >= 18446744073709551616.0 ? UINT64_MAX : (uint64_t)da);
        } else {
            ru = isnan(da) ? 0u : (da <= -9223372036854775808.0 ? (uint64_t)INT64_MIN : (da >= 9223372036854775808.0 ? (uint64_t)INT64_MAX : (uint64_t)(int64_t)da));
        }
        if (sret) shim_write_u64_ret(it, args[0].ptr, ru);
        else      pir_val_set_u64(out_ret, ret_ty, ru);
        return true;
    }
    return false;
}

/* 16-bit div/rem helper shim (targets without hardware div). */
static bool shim_hi3(PirInterp *it, const char *name, const PirVal *args, uint32_t arg_count,
                     PirType ret_ty, PirVal *out_ret) {
    if (arg_count != 2u) {
        pir_interp_fail(it, PIR_INTERP_ERR, "%s expects 2 args", name);
        return false;
    }
    uint16_t a = (uint16_t)pir_val_get_u64(&args[0]);
    uint16_t b = (uint16_t)pir_val_get_u64(&args[1]);
    if (b == 0u) {
        pir_interp_fail(it, PIR_INTERP_TRAP, "16-bit division by zero in %s", name);
        return false;
    }
    uint16_t res = 0u;
    if (strcmp(name, "__divhi3") == 0) {
        if ((int16_t)a == INT16_MIN && (int16_t)b == -1) res = (uint16_t)INT16_MIN;
        else res = (uint16_t)((int16_t)a / (int16_t)b);
    } else if (strcmp(name, "__modhi3") == 0) {
        if ((int16_t)a == INT16_MIN && (int16_t)b == -1) res = 0u;
        else res = (uint16_t)((int16_t)a % (int16_t)b);
    } else if (strcmp(name, "__udivhi3") == 0) {
        res = (uint16_t)(a / b);
    } else {
        res = (uint16_t)(a % b);
    }
    pir_val_set_u64(out_ret, ret_ty, res);
    return true;
}

/* 32-bit div/rem helper shim (16-bit targets): expects (out_ptr, a0, a1, b0, b1). */
static bool shim_si3(PirInterp *it, const char *name, const PirVal *args, uint32_t arg_count) {
    if (arg_count != 5u) {
        pir_interp_fail(it, PIR_INTERP_ERR, "%s expects 5 args", name);
        return false;
    }
    PirPtr out_p = args[0].ptr;
    uint32_t a = shim_pair_u32(it, &args[1], &args[2]);
    uint32_t b = shim_pair_u32(it, &args[3], &args[4]);
    if (b == 0u) {
        pir_interp_fail(it, PIR_INTERP_TRAP, "32-bit division by zero in %s", name);
        return false;
    }
    uint32_t res = 0u;
    if (strcmp(name, "__divsi3") == 0) {
        if ((int32_t)a == INT32_MIN && (int32_t)b == -1) res = (uint32_t)INT32_MIN;
        else res = (uint32_t)((int32_t)a / (int32_t)b);
    } else if (strcmp(name, "__modsi3") == 0) {
        if ((int32_t)a == INT32_MIN && (int32_t)b == -1) res = 0u;
        else res = (uint32_t)((int32_t)a % (int32_t)b);
    } else if (strcmp(name, "__udivsi3") == 0) {
        res = a / b;
    } else {
        res = a % b;
    }
    return shim_write_u32_ret(it, out_p, res);
}

/* 64-bit div/rem helper shim (32-bit or 16-bit targets). */
static bool shim_di3(PirInterp *it, const char *name, const PirVal *args, uint32_t arg_count) {
    if (arg_count != 5u && arg_count != 9u) {
        pir_interp_fail(it, PIR_INTERP_ERR, "%s expects 5 or 9 args", name);
        return false;
    }
    PirPtr out_p = args[0].ptr;
    uint64_t a = (arg_count == 5u) ? shim_pair_u64(it, &args[1], &args[2]) : shim_quad16_u64(it, &args[1]);
    uint64_t b = (arg_count == 5u) ? shim_pair_u64(it, &args[3], &args[4]) : shim_quad16_u64(it, &args[5]);
    if (b == 0u) {
        pir_interp_fail(it, PIR_INTERP_TRAP, "64-bit division by zero in %s", name);
        return false;
    }
    uint64_t res = 0u;
    if (strcmp(name, "__divdi3") == 0) {
        if ((int64_t)a == INT64_MIN && (int64_t)b == -1) res = (uint64_t)INT64_MIN;
        else res = (uint64_t)((int64_t)a / (int64_t)b);
    } else if (strcmp(name, "__moddi3") == 0) {
        if ((int64_t)a == INT64_MIN && (int64_t)b == -1) res = 0u;
        else res = (uint64_t)((int64_t)a % (int64_t)b);
    } else if (strcmp(name, "__udivdi3") == 0) {
        res = a / b;
    } else {
        res = a % b;
    }
    uint8_t buf[8];
    shim_encode_u64(it, buf, res);
    return pir_interp_mem_write(it, out_p, buf, 8u, (shim_max_int_bits(it) == 16u) ? 1u : 4u);
}

/* 128-bit div/rem helper shim: expects (out_ptr, a_lo, a_hi, b_lo, b_hi) writing 16B result. */
static bool shim_ti3(PirInterp *it, const char *name, const PirVal *args, uint32_t arg_count) {
    if (arg_count != 5u && arg_count != 9u && arg_count != 17u) {
        pir_interp_fail(it, PIR_INTERP_ERR, "%s expects 5, 9, or 17 args", name);
        return false;
    }
    PirPtr out_p  = args[0].ptr;
    uint64_t a_lo = 0u, a_hi = 0u, b_lo = 0u, b_hi = 0u;
    if (arg_count == 5u) {
        a_lo = pir_val_get_u64(&args[1]);
        a_hi = pir_val_get_u64(&args[2]);
        b_lo = pir_val_get_u64(&args[3]);
        b_hi = pir_val_get_u64(&args[4]);
    } else if (arg_count == 9u) {
        a_lo = shim_pair_u64(it, &args[1], &args[2]);
        a_hi = shim_pair_u64(it, &args[3], &args[4]);
        b_lo = shim_pair_u64(it, &args[5], &args[6]);
        b_hi = shim_pair_u64(it, &args[7], &args[8]);
    } else {
        a_lo = shim_quad16_u64(it, &args[1]);
        a_hi = shim_quad16_u64(it, &args[5]);
        b_lo = shim_quad16_u64(it, &args[9]);
        b_hi = shim_quad16_u64(it, &args[13]);
    }
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
    if (shim_is_be(it)) {
        shim_encode_u64(it, buf, res_hi);
        shim_encode_u64(it, buf + 8u, res_lo);
    } else {
        memcpy(buf, &res_lo, 8u);
        memcpy(buf + 8u, &res_hi, 8u);
    }
    return pir_interp_mem_write(it, out_p, buf, 16u, 8u);
}

bool pir_interp_try_shim(PirInterp *it, const char *name,
                         const PirVal *args, uint32_t arg_count,
                         PirType ret_ty, PirVal *out_ret) {
    memset(out_ret, 0, sizeof(*out_ret));
    out_ret->ty = ret_ty;

    if (shim_try_soft_float(it, name, args, arg_count, ret_ty, out_ret)) {
        return true;
    }

    uint32_t mib = shim_max_int_bits(it);

    if (strcmp(name, "malloc") == 0) {
        uint64_t sz = 0u;
        if (arg_count == 1u) {
            sz = pir_val_get_u64(&args[0]);
        } else if (arg_count == 2u && mib == 32u) {
            sz = shim_pair_u64(it, &args[0], &args[1]);
        } else if (arg_count == 2u && mib == 16u) {
            sz = (uint64_t)shim_pair_u32(it, &args[0], &args[1]);
        } else if (arg_count == 4u && mib == 16u) {
            sz = shim_quad16_u64(it, &args[0]);
        } else {
            pir_interp_fail(it, PIR_INTERP_UB, "malloc arity");
            return true;
        }
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
        uint64_t n = 0u, s = 0u;
        if (arg_count == 2u) {
            n = pir_val_get_u64(&args[0]); s = pir_val_get_u64(&args[1]);
        } else if (arg_count == 4u && mib == 32u) {
            n = shim_pair_u64(it, &args[0], &args[1]); s = shim_pair_u64(it, &args[2], &args[3]);
        } else if (arg_count == 4u && mib == 16u) {
            n = shim_pair_u32(it, &args[0], &args[1]); s = shim_pair_u32(it, &args[2], &args[3]);
        } else if (arg_count == 8u && mib == 16u) {
            n = shim_quad16_u64(it, &args[0]); s = shim_quad16_u64(it, &args[4]);
        } else {
            pir_interp_fail(it, PIR_INTERP_UB, "calloc arity");
            return true;
        }
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
        if (pir_ptr_is_null(p)) return true; /* free(NULL) is a no-op */
        if (p.alloc_id == 0u || p.alloc_id >= it->allocs.len) {
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
        uint64_t new_sz_u64 = 0u;
        if (arg_count == 2u) {
            new_sz_u64 = pir_val_get_u64(&args[1]);
        } else if (arg_count == 3u && mib == 32u) {
            new_sz_u64 = shim_pair_u64(it, &args[1], &args[2]);
        } else if (arg_count == 3u && mib == 16u) {
            new_sz_u64 = shim_pair_u32(it, &args[1], &args[2]);
        } else if (arg_count == 5u && mib == 16u) {
            new_sz_u64 = shim_quad16_u64(it, &args[1]);
        } else {
            pir_interp_fail(it, PIR_INTERP_UB, "realloc arity");
            return true;
        }
        PirPtr p = args[0].ptr;
        if (pir_ptr_is_null(p)) {
            if (new_sz_u64 > (UINT64_C(1) << 30)) {
                out_ret->ptr.alloc_id = 0u;
                out_ret->ptr.offset   = 0;
                return true;
            }
            uint32_t id = pir_interp_alloc_new(it, (size_t)new_sz_u64, 16u, false, true, false, false);
            out_ret->ptr.alloc_id = id;
            out_ret->ptr.offset   = 0;
            return true;
        }
        if (p.alloc_id == 0u || p.alloc_id >= it->allocs.len) {
            pir_interp_fail(it, PIR_INTERP_UB, "realloc of invalid pointer");
            return true;
        }
        PirAlloc *old_a = &it->allocs.data[p.alloc_id];
        if (!old_a->is_heap || !old_a->alive || p.offset != 0) {
            pir_interp_fail(it, PIR_INTERP_UB, "realloc of dead/non-heap/interior pointer");
            return true;
        }
        if (new_sz_u64 > (UINT64_C(1) << 30)) {
            out_ret->ptr.alloc_id = 0u;
            out_ret->ptr.offset   = 0;
            return true;
        }
        size_t new_sz = (size_t)new_sz_u64;
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
        size_t len = 0u;
        if (arg_count == 3u) len = (size_t)pir_val_get_u64(&args[2]);
        else if (arg_count == 4u && mib == 32u) len = (size_t)shim_pair_u64(it, &args[2], &args[3]);
        else if (arg_count == 4u && mib == 16u) len = (size_t)shim_pair_u32(it, &args[2], &args[3]);
        else if (arg_count == 6u && mib == 16u) len = (size_t)shim_quad16_u64(it, &args[2]);
        else { pir_interp_fail(it, PIR_INTERP_UB, "memcpy/memmove arity"); return true; }
        pir_interp_mem_copy(it, args[0].ptr, args[1].ptr, len, strcmp(name, "memmove") == 0);
        out_ret->ptr = args[0].ptr;
        return true;
    }
    if (strcmp(name, "memset") == 0) {
        PirPtr dst = args[0].ptr;
        uint8_t byte = 0u;
        size_t len = 0u;
        if (arg_count == 3u) {
            byte = (uint8_t)pir_val_get_u64(&args[1]);
            len = (size_t)pir_val_get_u64(&args[2]);
        } else if (arg_count == 4u && mib == 32u) {
            byte = (uint8_t)pir_val_get_u64(&args[1]);
            len = (size_t)shim_pair_u64(it, &args[2], &args[3]);
        } else if (arg_count == 4u && mib == 16u) {
            if (args[1].ty.kind == PIR_TYPE_SCALAR && args[1].ty.u.scalar == PIR_SCALAR_I8) {
                byte = (uint8_t)pir_val_get_u64(&args[1]);
                len  = (size_t)shim_pair_u32(it, &args[2], &args[3]);
            } else {
                byte = (uint8_t)shim_pair_u32(it, &args[1], &args[2]);
                len  = (size_t)pir_val_get_u64(&args[3]);
            }
        } else if (arg_count == 5u && mib == 16u) {
            byte = (uint8_t)shim_pair_u32(it, &args[1], &args[2]);
            len = (size_t)shim_pair_u32(it, &args[3], &args[4]);
        } else if (arg_count == 6u && mib == 16u) {
            byte = (uint8_t)pir_val_get_u64(&args[1]);
            len = (size_t)shim_quad16_u64(it, &args[2]);
        } else if (arg_count == 7u && mib == 16u) {
            byte = (uint8_t)shim_pair_u32(it, &args[1], &args[2]);
            len = (size_t)shim_quad16_u64(it, &args[3]);
        } else {
            pir_interp_fail(it, PIR_INTERP_UB, "memset arity");
            return true;
        }
        if (len > 0u && pir_interp_check_access(it, dst, len, 1u, true, false)) {
            PirAlloc *a = &it->allocs.data[dst.alloc_id];
            size_t off = (size_t)dst.offset;
            memset(a->bytes + off, byte, len);
            memset(a->init_bits + off, 1, len);
            pir_interp_clear_ptr_tags(a, off, len, it->ptr_bytes);
        }
        out_ret->ptr = dst;
        return true;
    }
    if (strcmp(name, "memcmp") == 0) {
        bool sret = (mib == 16u && shim_is_void_ret(ret_ty));
        uint32_t base = sret ? 1u : 0u;
        uint32_t rem = (arg_count >= base) ? (arg_count - base) : 0u;
        size_t len = 0u;
        if (rem == 3u) len = (size_t)pir_val_get_u64(&args[base + 2u]);
        else if (rem == 4u && mib == 32u) len = (size_t)shim_pair_u64(it, &args[base + 2u], &args[base + 3u]);
        else if (rem == 4u && mib == 16u) len = (size_t)shim_pair_u32(it, &args[base + 2u], &args[base + 3u]);
        else if (rem == 6u && mib == 16u) len = (size_t)shim_quad16_u64(it, &args[base + 2u]);
        else { pir_interp_fail(it, PIR_INTERP_UB, "memcmp arity"); return true; }
        int res = 0;
        if (len > 0u &&
            pir_interp_check_access(it, args[base].ptr, len, 1u, false, true) &&
            pir_interp_check_access(it, args[base + 1u].ptr, len, 1u, false, true)) {
            const uint8_t *b1 = it->allocs.data[args[base].ptr.alloc_id].bytes + args[base].ptr.offset;
            const uint8_t *b2 = it->allocs.data[args[base + 1u].ptr.alloc_id].bytes + args[base + 1u].ptr.offset;
            res = memcmp(b1, b2, len);
        }
        if (sret) shim_write_u32_ret(it, args[0].ptr, (uint32_t)(int32_t)res);
        else      pir_val_set_u64(out_ret, ret_ty, (uint64_t)(int64_t)res);
        return true;
    }
    if (strcmp(name, "strlen") == 0) {
        bool sp_out = (arg_count == 2u && mib < 64u && shim_is_void_ret(ret_ty));
        if (arg_count != 1u && !sp_out) { pir_interp_fail(it, PIR_INTERP_UB, "strlen arity"); return true; }
        char *s = NULL;
        size_t len = 0u;
        PirPtr sp = sp_out ? args[1].ptr : args[0].ptr;
        if (read_cstr(it, sp, &s, &len)) {
            free(s);
            if (sp_out) {
                PirAlloc *oa = (args[0].ptr.alloc_id < it->allocs.len) ? &it->allocs.data[args[0].ptr.alloc_id] : NULL;
                size_t avail = (oa && args[0].ptr.offset >= 0 && (size_t)args[0].ptr.offset <= oa->size)
                               ? (oa->size - (size_t)args[0].ptr.offset) : 8u;
                if (avail >= 8u) {
                    shim_write_u64_ret(it, args[0].ptr, (uint64_t)len);
                } else {
                    shim_write_u32_ret(it, args[0].ptr, (uint32_t)len);
                }
            } else {
                pir_val_set_u64(out_ret, ret_ty, (uint64_t)len);
            }
        }
        return true;
    }
    if (strcmp(name, "putchar") == 0) {
        bool sret = (mib == 16u && shim_is_void_ret(ret_ty));
        uint32_t base = sret ? 1u : 0u;
        if (arg_count < base + 1u) { pir_interp_fail(it, PIR_INTERP_UB, "putchar arity"); return true; }
        int c = (int)(uint8_t)pir_val_get_u64(&args[base]);
        int r = putchar(c);
        fflush(stdout);
        if (sret) shim_write_u32_ret(it, args[0].ptr, (uint32_t)(int32_t)r);
        else      pir_val_set_u64(out_ret, ret_ty, (uint64_t)(int64_t)r);
        return true;
    }
    if (strcmp(name, "puts") == 0) {
        bool sret = (mib == 16u && shim_is_void_ret(ret_ty));
        uint32_t base = sret ? 1u : 0u;
        if (arg_count != base + 1u) { pir_interp_fail(it, PIR_INTERP_UB, "puts arity"); return true; }
        char *s = NULL;
        if (read_cstr(it, args[base].ptr, &s, NULL)) {
            int r = puts(s);
            fflush(stdout);
            free(s);
            if (sret) shim_write_u32_ret(it, args[0].ptr, (uint32_t)(int32_t)r);
            else      pir_val_set_u64(out_ret, ret_ty, (uint64_t)(int64_t)r);
        }
        return true;
    }
    if (strcmp(name, "printf") == 0) {
        bool sret = (mib == 16u && shim_is_void_ret(ret_ty));
        int written = 0;
        const PirVal *pargs = sret ? (args + 1u) : args;
        uint32_t pcnt = sret ? (arg_count > 0u ? arg_count - 1u : 0u) : arg_count;
        if (shim_printf(it, pargs, pcnt, &written)) {
            if (sret) shim_write_u32_ret(it, args[0].ptr, (uint32_t)(int32_t)written);
            else      pir_val_set_u64(out_ret, ret_ty, (uint64_t)(int64_t)written);
        }
        return true;
    }
    if (strcmp(name, "abort") == 0) {
        pir_interp_fail(it, PIR_INTERP_TRAP, "abort() called");
        return true;
    }
    if (strcmp(name, "exit") == 0) {
        int code = 0;
        if (arg_count == 2u && mib == 16u) code = (int)(int32_t)shim_pair_u32(it, &args[0], &args[1]);
        else if (arg_count >= 1u) code = (int)(int32_t)pir_val_get_u64(&args[0]);
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
        PirPtr exn_slot, jb_slot;
        if (!pir_ptr_offset_checked(it, frame_p, (int64_t)it->ptr_bytes, &exn_slot) ||
            !pir_ptr_offset_checked(it, frame_p, (int64_t)(2u * it->ptr_bytes), &jb_slot)) {
            return true;
        }
        if (!pir_interp_mem_write_ptr(it, exn_slot, exn, it->ptr_bytes)) {
            return true;
        }
        it->unwinding        = true;
        it->unwind_target_jb = jb_slot;
        it->unwind_exn       = exn;
        return true;
    }
    if (strcmp(name, "__divhi3") == 0 || strcmp(name, "__udivhi3") == 0 ||
        strcmp(name, "__modhi3") == 0 || strcmp(name, "__umodhi3") == 0) {
        shim_hi3(it, name, args, arg_count, ret_ty, out_ret);
        return true;
    }
    if (strcmp(name, "__divsi3") == 0 || strcmp(name, "__udivsi3") == 0 ||
        strcmp(name, "__modsi3") == 0 || strcmp(name, "__umodsi3") == 0) {
        shim_si3(it, name, args, arg_count);
        return true;
    }
    if (strcmp(name, "__divti3") == 0 || strcmp(name, "__udivti3") == 0 ||
        strcmp(name, "__modti3") == 0 || strcmp(name, "__umodti3") == 0) {
        shim_ti3(it, name, args, arg_count);
        return true;
    }
    if (strcmp(name, "__divdi3") == 0 || strcmp(name, "__udivdi3") == 0 ||
        strcmp(name, "__moddi3") == 0 || strcmp(name, "__umoddi3") == 0) {
        shim_di3(it, name, args, arg_count);
        return true;
    }
    if (strcmp(name, "__pir_atomic_load_4") == 0 && arg_count == 2u) {
        uint8_t buf[4];
        if (pir_interp_mem_read(it, args[0].ptr, buf, 4u, 1u)) {
            pir_interp_mem_write(it, args[1].ptr, buf, 4u, 1u);
        }
        return true;
    }
    if (strcmp(name, "__pir_atomic_store_4") == 0 && arg_count == 3u) {
        uint32_t v = (uint32_t)(uint16_t)pir_val_get_u64(&args[1]) |
                     ((uint32_t)(uint16_t)pir_val_get_u64(&args[2]) << 16u);
        uint8_t buf[4];
        shim_encode_u32(it, buf, v);
        pir_interp_mem_write(it, args[0].ptr, buf, 4u, 1u);
        return true;
    }
    if (strcmp(name, "__pir_atomic_rmw_4") == 0 && arg_count == 5u) {
        uint8_t buf[4];
        if (pir_interp_mem_read(it, args[0].ptr, buf, 4u, 1u)) {
            uint32_t oldv = shim_decode_u32(it, buf);
            pir_interp_mem_write(it, args[1].ptr, buf, 4u, 1u);
            uint32_t op = (uint32_t)pir_val_get_u64(&args[2]);
            uint32_t rhs = (uint32_t)(uint16_t)pir_val_get_u64(&args[3]) |
                           ((uint32_t)(uint16_t)pir_val_get_u64(&args[4]) << 16u);
            uint32_t nv = oldv;
            switch ((PirRmwOp)op) {
                case PIR_RMW_ADD:  nv = oldv + rhs; break;
                case PIR_RMW_SUB:  nv = oldv - rhs; break;
                case PIR_RMW_AND:  nv = oldv & rhs; break;
                case PIR_RMW_OR:   nv = oldv | rhs; break;
                case PIR_RMW_XOR:  nv = oldv ^ rhs; break;
                case PIR_RMW_XCHG: nv = rhs;        break;
            }
            shim_encode_u32(it, buf, nv);
            pir_interp_mem_write(it, args[0].ptr, buf, 4u, 1u);
        }
        return true;
    }
    if (strcmp(name, "__pir_atomic_cmpxchg_4") == 0 && arg_count == 6u) {
        uint8_t buf[4];
        if (pir_interp_mem_read(it, args[0].ptr, buf, 4u, 1u)) {
            uint32_t oldv = shim_decode_u32(it, buf);
            pir_interp_mem_write(it, args[1].ptr, buf, 4u, 1u);
            uint32_t expv = (uint32_t)(uint16_t)pir_val_get_u64(&args[2]) |
                            ((uint32_t)(uint16_t)pir_val_get_u64(&args[3]) << 16u);
            uint32_t newv = (uint32_t)(uint16_t)pir_val_get_u64(&args[4]) |
                            ((uint32_t)(uint16_t)pir_val_get_u64(&args[5]) << 16u);
            bool eq = (oldv == expv);
            if (eq) {
                shim_encode_u32(it, buf, newv);
                pir_interp_mem_write(it, args[0].ptr, buf, 4u, 1u);
            }
            pir_val_set_u64(out_ret, ret_ty, eq ? 1u : 0u);
        }
        return true;
    }
    if (strcmp(name, "__pir_atomic_load_8") == 0 && arg_count == 2u) {
        uint8_t buf[8];
        uint32_t al = (mib == 16u) ? 1u : 8u;
        if (pir_interp_mem_read(it, args[0].ptr, buf, 8u, al)) {
            pir_interp_mem_write(it, args[1].ptr, buf, 8u, (mib == 16u) ? 1u : 4u);
        }
        return true;
    }
    if (strcmp(name, "__pir_atomic_store_8") == 0 && (arg_count == 3u || arg_count == 5u)) {
        uint64_t v = (arg_count == 3u)
                     ? ((uint64_t)(uint32_t)pir_val_get_u64(&args[1]) |
                        ((uint64_t)(uint32_t)pir_val_get_u64(&args[2]) << 32u))
                     : shim_quad16_u64(it, &args[1]);
        uint8_t buf[8];
        shim_encode_u64(it, buf, v);
        pir_interp_mem_write(it, args[0].ptr, buf, 8u, (mib == 16u) ? 1u : 8u);
        return true;
    }
    if (strcmp(name, "__pir_atomic_rmw_8") == 0 && (arg_count == 5u || arg_count == 8u)) {
        uint8_t buf[8];
        uint32_t al = (mib == 16u) ? 1u : 8u;
        if (pir_interp_mem_read(it, args[0].ptr, buf, 8u, al)) {
            uint64_t oldv = shim_decode_u64(it, buf);
            pir_interp_mem_write(it, args[1].ptr, buf, 8u, (mib == 16u) ? 1u : 4u);
            uint32_t op = (arg_count == 5u) ? (uint32_t)pir_val_get_u64(&args[2])
                                            : shim_pair_u32(it, &args[2], &args[3]);
            uint64_t rhs = (arg_count == 5u)
                           ? ((uint64_t)(uint32_t)pir_val_get_u64(&args[3]) |
                              ((uint64_t)(uint32_t)pir_val_get_u64(&args[4]) << 32u))
                           : shim_quad16_u64(it, &args[4]);
            uint64_t nv = oldv;
            switch ((PirRmwOp)op) {
                case PIR_RMW_ADD:  nv = oldv + rhs; break;
                case PIR_RMW_SUB:  nv = oldv - rhs; break;
                case PIR_RMW_AND:  nv = oldv & rhs; break;
                case PIR_RMW_OR:   nv = oldv | rhs; break;
                case PIR_RMW_XOR:  nv = oldv ^ rhs; break;
                case PIR_RMW_XCHG: nv = rhs;        break;
            }
            shim_encode_u64(it, buf, nv);
            pir_interp_mem_write(it, args[0].ptr, buf, 8u, al);
        }
        return true;
    }
    if (strcmp(name, "__pir_atomic_cmpxchg_8") == 0 && (arg_count == 6u || arg_count == 10u)) {
        uint8_t buf[8];
        uint32_t al = (mib == 16u) ? 1u : 8u;
        if (pir_interp_mem_read(it, args[0].ptr, buf, 8u, al)) {
            uint64_t oldv = shim_decode_u64(it, buf);
            pir_interp_mem_write(it, args[1].ptr, buf, 8u, (mib == 16u) ? 1u : 4u);
            uint64_t expv = (arg_count == 6u)
                            ? ((uint64_t)(uint32_t)pir_val_get_u64(&args[2]) |
                               ((uint64_t)(uint32_t)pir_val_get_u64(&args[3]) << 32u))
                            : shim_quad16_u64(it, &args[2]);
            uint64_t newv = (arg_count == 6u)
                            ? ((uint64_t)(uint32_t)pir_val_get_u64(&args[4]) |
                               ((uint64_t)(uint32_t)pir_val_get_u64(&args[5]) << 32u))
                            : shim_quad16_u64(it, &args[6]);
            bool eq = (oldv == expv);
            if (eq) {
                shim_encode_u64(it, buf, newv);
                pir_interp_mem_write(it, args[0].ptr, buf, 8u, al);
            }
            pir_val_set_u64(out_ret, ret_ty, eq ? 1u : 0u);
        }
        return true;
    }
    return false;
}
