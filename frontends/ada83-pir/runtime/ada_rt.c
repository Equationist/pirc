/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Minimal C99 Ada 83 Runtime for diana2pir + pirc.
 * Provides:
 *   - Standard Ada exception tags & unhandled exception handler
 *   - Calendar (Clock in nanoseconds via CLOCK_REALTIME)
 *   - 'Image / 'Value, exponentiation, fixed-point, heap & secondary stack helpers
 */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#elif defined(__linux__)
#define _GNU_SOURCE
#endif
#ifdef __APPLE__
#include <sys/ucontext.h>
#elif defined(__linux__)
#include <ucontext.h>
#endif
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* External PIR shadow-stack unwind entrypoint from runtime/pir_rt.c */
extern void __pir_unwind_resume(void *exn);

/* Predefined Standard / IO_Exceptions / Calendar exception identity tags.
 * The address of each string serves as the unique exception pointer identity. */
const char __ada_exc_constraint_error[] = "CONSTRAINT_ERROR";
const char __ada_exc_numeric_error[]    = "NUMERIC_ERROR";
const char __ada_exc_program_error[]    = "PROGRAM_ERROR";
const char __ada_exc_storage_error[]    = "STORAGE_ERROR";
const char __ada_exc_tasking_error[]    = "TASKING_ERROR";
const char __ada_exc_status_error[]     = "IO_EXCEPTIONS.STATUS_ERROR";
const char __ada_exc_mode_error[]       = "IO_EXCEPTIONS.MODE_ERROR";
const char __ada_exc_name_error[]       = "IO_EXCEPTIONS.NAME_ERROR";
const char __ada_exc_use_error[]        = "IO_EXCEPTIONS.USE_ERROR";
const char __ada_exc_device_error[]     = "IO_EXCEPTIONS.DEVICE_ERROR";
const char __ada_exc_end_error[]        = "IO_EXCEPTIONS.END_ERROR";
const char __ada_exc_data_error[]       = "IO_EXCEPTIONS.DATA_ERROR";
const char __ada_exc_layout_error[]     = "IO_EXCEPTIONS.LAYOUT_ERROR";
const char __ada_exc_time_error[]       = "CALENDAR.TIME_ERROR";

__attribute__((noreturn))
void __ada_raise(const void *exc_tag) {
    __pir_unwind_resume((void *)exc_tag);
    abort();
}

__attribute__((noreturn))
void __ada_unhandled_exception(const void *exc_tag) {
    const char *name = exc_tag ? (const char *)exc_tag : "UNKNOWN_EXCEPTION";
    fprintf(stderr, "raised %s\n", name);
    fflush(stderr);
    exit(1);
}

/* =========================================================================
 * Calendar Runtime Support (Ada 83 TIME = nanoseconds since 1901-01-01 UTC)
 * ========================================================================= */

static int ada_is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int ada_days_in_month(int y, int m) {
    static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && ada_is_leap(y)) return 29;
    return md[m - 1];
}

/* Seconds between 1901-01-01 00:00:00 UTC and 1970-01-01 00:00:00 UTC:
 * 69 years including 17 leap years (1904..1968) = 25202 days = 2177452800 s.
 * STANDARD.DURATION has small = 10^(-9) s (1 ns), so 1 second = 10^9 ticks.
 * 199 years (1901..2099 inclusive = 72684 days) * 86400 * 10^9 = 6.2799e18 < INT64_MAX. */
#define ADA_EPOCH_OFFSET_SECS 2177452800LL
#define ADA_TICKS_PER_SEC     1000000000LL
#define ADA_TICKS_PER_DAY     (86400LL * ADA_TICKS_PER_SEC)
#define ADA_TOTAL_DAYS_2099   72684LL
#define ADA_MAX_TIME_TICKS    (ADA_TOTAL_DAYS_2099 * ADA_TICKS_PER_DAY)

int64_t __ada_clock_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        __ada_raise(__ada_exc_time_error);
    }
    __int128 total = ((__int128)ts.tv_sec + (__int128)ADA_EPOCH_OFFSET_SECS) * (__int128)ADA_TICKS_PER_SEC
                     + (__int128)ts.tv_nsec;
    if (total < 0 || total >= (__int128)ADA_MAX_TIME_TICKS) {
        __ada_raise(__ada_exc_time_error);
    }
    return (int64_t)total;
}

int64_t __ada_cal_time_of(int32_t year, int32_t month, int32_t day, int64_t seconds_ticks) {
    if (year < 1901 || year > 2099 || month < 1 || month > 12) {
        __ada_raise(__ada_exc_time_error);
    }
    int dim = ada_days_in_month(year, month);
    if (day < 1 || day > dim || seconds_ticks < 0 || seconds_ticks > ADA_TICKS_PER_DAY) {
        __ada_raise(__ada_exc_time_error);
    }
    int64_t days = 0;
    for (int y = 1901; y < year; ++y) {
        days += ada_is_leap(y) ? 366 : 365;
    }
    for (int m = 1; m < month; ++m) {
        days += ada_days_in_month(year, m);
    }
    days += (day - 1);
    if (seconds_ticks == ADA_TICKS_PER_DAY) {
        days += 1;
        seconds_ticks = 0;
    }
    if (days >= ADA_TOTAL_DAYS_2099) {
        __ada_raise(__ada_exc_time_error);
    }
    return days * ADA_TICKS_PER_DAY + seconds_ticks;
}

void __ada_cal_split(int64_t date_ticks, int32_t *year_out, int32_t *month_out,
                     int32_t *day_out, int64_t *sec_out) {
    if (date_ticks < 0 || date_ticks >= ADA_MAX_TIME_TICKS) {
        __ada_raise(__ada_exc_time_error);
    }
    int64_t days = date_ticks / ADA_TICKS_PER_DAY;
    int64_t rem  = date_ticks % ADA_TICKS_PER_DAY;
    int y = 1901;
    while (y <= 2099) {
        int dy = ada_is_leap(y) ? 366 : 365;
        if (days < dy) break;
        days -= dy;
        y++;
    }
    if (y > 2099) {
        __ada_raise(__ada_exc_time_error);
    }
    int m = 1;
    while (m <= 12) {
        int dm = ada_days_in_month(y, m);
        if (days < dm) break;
        days -= dm;
        m++;
    }
    if (year_out)  *year_out  = (int32_t)y;
    if (month_out) *month_out = (int32_t)m;
    if (day_out)   *day_out   = (int32_t)(days + 1);
    if (sec_out)   *sec_out   = rem;
}

int64_t __ada_cal_add(int64_t t, int64_t d) {
    __int128 res = (__int128)t + (__int128)d;
    if (res < 0 || res >= (__int128)ADA_MAX_TIME_TICKS) {
        __ada_raise(__ada_exc_time_error);
    }
    return (int64_t)res;
}

int64_t __ada_cal_sub_dur(int64_t t, int64_t d) {
    __int128 res = (__int128)t - (__int128)d;
    if (res < 0 || res >= (__int128)ADA_MAX_TIME_TICKS) {
        __ada_raise(__ada_exc_time_error);
    }
    return (int64_t)res;
}

int64_t __ada_cal_sub_time(int64_t t1, int64_t t2) {
    /* Result must fit within STANDARD.DURATION (-86400.0 .. +86400.0) */
    __int128 diff = (__int128)t1 - (__int128)t2;
    if (diff < -(__int128)ADA_TICKS_PER_DAY || diff > (__int128)ADA_TICKS_PER_DAY) {
        __ada_raise(__ada_exc_time_error);
    }
    return (int64_t)diff;
}

int32_t __ada_cal_year(int64_t date_ns) {
    int32_t y = 1901;
    __ada_cal_split(date_ns, &y, NULL, NULL, NULL);
    return y;
}

int32_t __ada_cal_month(int64_t date_ns) {
    int32_t m = 1;
    __ada_cal_split(date_ns, NULL, &m, NULL, NULL);
    return m;
}

int32_t __ada_cal_day(int64_t date_ns) {
    int32_t d = 1;
    __ada_cal_split(date_ns, NULL, NULL, &d, NULL);
    return d;
}

int64_t __ada_cal_seconds(int64_t date_ns) {
    int64_t s = 0;
    __ada_cal_split(date_ns, NULL, NULL, NULL, &s);
    return s;
}

/* =========================================================================
 * 'Image and 'Value Attribute Runtime Support
 * ========================================================================= */

static const char *const g_ascii_ctrl_names[32] = {
    "NUL","SOH","STX","ETX","EOT","ENQ","ACK","BEL",
    "BS","HT","LF","VT","FF","CR","SO","SI",
    "DLE","DC1","DC2","DC3","DC4","NAK","SYN","ETB",
    "CAN","EM","SUB","ESC","FS","GS","RS","US"
};

int32_t __ada_cstr_len(const char *s) {
    return s ? (int32_t)strlen(s) : 0;
}

int32_t __ada_img_i64(int64_t val, char *buf) {
    if (!buf) return 0;
    int n = (val >= 0) ? snprintf(buf, 32, " %" PRId64, val)
                       : snprintf(buf, 32, "%" PRId64, val);
    if (n < 0) return 0;
    return (n < 32) ? (int32_t)n : 31;
}

int32_t __ada_img_char(int8_t c, char *buf) {
    unsigned char uc = (unsigned char)c;
    if (uc < 32) {
        const char *nm = g_ascii_ctrl_names[uc];
        int32_t n = (int32_t)strlen(nm);
        memcpy(buf, nm, (size_t)n);
        return n;
    } else if (uc == 127) {
        memcpy(buf, "DEL", 3);
        return 3;
    } else {
        buf[0] = '\'';
        buf[1] = (char)uc;
        buf[2] = '\'';
        return 3;
    }
}

static int ada_is_space(unsigned char c) {
    return c == ' ';
}

static int ada_ci_eq(const char *a, int32_t alen, const char *b) {
    int32_t blen = (int32_t)strlen(b);
    if (alen != blen) return 0;
    for (int32_t i = 0; i < alen; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
    }
    return 1;
}

int8_t __ada_val_char(const char *ptr, int32_t len) {
    if (!ptr || len <= 0) __ada_raise(__ada_exc_constraint_error);
    int32_t s = 0, e = len;
    while (s < e && ada_is_space((unsigned char)ptr[s])) s++;
    while (e > s && ada_is_space((unsigned char)ptr[e - 1])) e--;
    int32_t n = e - s;
    if (n == 3 && ptr[s] == '\'' && ptr[s + 2] == '\'') {
        unsigned char uc = (unsigned char)ptr[s + 1];
        if (uc >= 32 && uc <= 126) return (int8_t)uc;
        __ada_raise(__ada_exc_constraint_error);
    }
    for (int i = 0; i < 32; ++i) {
        if (ada_ci_eq(ptr + s, n, g_ascii_ctrl_names[i])) return (int8_t)i;
    }
    if (ada_ci_eq(ptr + s, n, "DEL")) return (int8_t)127;
    __ada_raise(__ada_exc_constraint_error);
}

int32_t __ada_val_enum(const char *ptr, int32_t len, const char *const *table, int32_t count) {
    if (!ptr || len <= 0 || !table || count <= 0) __ada_raise(__ada_exc_constraint_error);
    int32_t s = 0, e = len;
    while (s < e && ada_is_space((unsigned char)ptr[s])) s++;
    while (e > s && ada_is_space((unsigned char)ptr[e - 1])) e--;
    int32_t n = e - s;
    if (n <= 0) __ada_raise(__ada_exc_constraint_error);
    int is_char_lit = (n == 3 && ptr[s] == '\'' && ptr[s + 2] == '\'');
    for (int32_t i = 0; i < count; ++i) {
        if (is_char_lit) {
            if (strlen(table[i]) == 3 && memcmp(ptr + s, table[i], 3) == 0) return i;
        } else {
            if (ada_ci_eq(ptr + s, n, table[i])) return i;
        }
    }
    __ada_raise(__ada_exc_constraint_error);
}

static int ada_digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int64_t __ada_val_i64(const char *ptr, int32_t len) {
    if (!ptr || len <= 0) __ada_raise(__ada_exc_constraint_error);
    int32_t s = 0, e = len;
    while (s < e && ada_is_space((unsigned char)ptr[s])) s++;
    while (e > s && ada_is_space((unsigned char)ptr[e - 1])) e--;
    if (s >= e) __ada_raise(__ada_exc_constraint_error);

    int neg = 0;
    if (ptr[s] == '+') { s++; }
    else if (ptr[s] == '-') { neg = 1; s++; }
    if (s >= e) __ada_raise(__ada_exc_constraint_error);

    /* Parse leading decimal integer (either value or base) */
    __int128 acc = 0;
    int prev_us = 1; /* disallow leading '_' */
    int32_t i = s;
    while (i < e && ((ptr[i] >= '0' && ptr[i] <= '9') || ptr[i] == '_')) {
        if (ptr[i] == '_') {
            if (prev_us) __ada_raise(__ada_exc_constraint_error);
            prev_us = 1;
        } else {
            acc = acc * 10 + (ptr[i] - '0');
            if (acc > ((__int128)1 << 65)) __ada_raise(__ada_exc_constraint_error);
            prev_us = 0;
        }
        i++;
    }
    if (prev_us) __ada_raise(__ada_exc_constraint_error);

    int64_t base = 10;
    if (i < e && (ptr[i] == '#' || ptr[i] == ':')) {
        char delim = ptr[i++];
        if (acc < 2 || acc > 16) __ada_raise(__ada_exc_constraint_error);
        base = (int64_t)acc;
        acc = 0;
        prev_us = 1;
        int got = 0;
        while (i < e && ptr[i] != delim) {
            if (ptr[i] == '_') {
                if (prev_us) __ada_raise(__ada_exc_constraint_error);
                prev_us = 1;
            } else {
                int dv = ada_digit_val(ptr[i]);
                if (dv < 0 || dv >= base) __ada_raise(__ada_exc_constraint_error);
                acc = acc * base + dv;
                if (acc > ((__int128)1 << 65)) __ada_raise(__ada_exc_constraint_error);
                prev_us = 0;
                got = 1;
            }
            i++;
        }
        if (!got || prev_us || i >= e || ptr[i] != delim) __ada_raise(__ada_exc_constraint_error);
        i++;
    }

    /* Optional exponent (must be non-negative for integer literals!) */
    if (i < e && (ptr[i] == 'E' || ptr[i] == 'e')) {
        i++;
        if (i < e && ptr[i] == '+') i++;
        else if (i < e && ptr[i] == '-') __ada_raise(__ada_exc_constraint_error);
        int64_t expv = 0;
        prev_us = 1;
        int got = 0;
        while (i < e) {
            if (ptr[i] == '_') {
                if (prev_us) __ada_raise(__ada_exc_constraint_error);
                prev_us = 1;
            } else if (ptr[i] >= '0' && ptr[i] <= '9') {
                if (expv < 1000000) expv = expv * 10 + (ptr[i] - '0');
                if (expv > 1000 && acc != 0) __ada_raise(__ada_exc_constraint_error);
                prev_us = 0;
                got = 1;
            } else {
                __ada_raise(__ada_exc_constraint_error);
            }
            i++;
        }
        if (!got || prev_us) __ada_raise(__ada_exc_constraint_error);
        if (acc != 0) {
            for (int64_t k = 0; k < expv; ++k) {
                acc *= base;
                if (acc > ((__int128)1 << 64)) __ada_raise(__ada_exc_constraint_error);
            }
        }
    }

    if (i != e) __ada_raise(__ada_exc_constraint_error);
    if (neg) acc = -acc;
    if (acc < (__int128)INT64_MIN || acc > (__int128)INT64_MAX) {
        __ada_raise(__ada_exc_constraint_error);
    }
    return (int64_t)acc;
}

/* =========================================================================
 * Exponentiation & Allocation Helpers
 * ========================================================================= */

int64_t __ada_ipow_i64(int64_t base, int32_t exp, int32_t checked) {
    if (exp < 0) {
        __ada_raise(__ada_exc_constraint_error);
    }
    int64_t res = 1;
    int64_t b = base;
    int32_t e = exp;
    while (e > 0) {
        if (e & 1) {
            if (checked) {
                __int128 prod = (__int128)res * (__int128)b;
                if (prod < INT64_MIN || prod > INT64_MAX) {
                    __ada_raise(__ada_exc_constraint_error);
                }
                res = (int64_t)prod;
            } else {
                res = (int64_t)((uint64_t)res * (uint64_t)b);
            }
        }
        e >>= 1;
        if (e > 0) {
            if (checked) {
                __int128 prod = (__int128)b * (__int128)b;
                if (prod < INT64_MIN || prod > INT64_MAX) {
                    __ada_raise(__ada_exc_constraint_error);
                }
                b = (int64_t)prod;
            } else {
                b = (int64_t)((uint64_t)b * (uint64_t)b);
            }
        }
    }
    return res;
}

int32_t __ada_ipow_i32(int32_t base, int32_t exp, int32_t checked) {
    if (exp < 0) {
        __ada_raise(__ada_exc_constraint_error);
    }
    int64_t r = __ada_ipow_i64((int64_t)base, exp, checked);
    if (checked && (r < INT32_MIN || r > INT32_MAX)) {
        __ada_raise(__ada_exc_constraint_error);
    }
    return (int32_t)r;
}

double __ada_fpow_f64(double base, int32_t exp) {
    double res = 1.0;
    double b = base;
    int64_t e = (int64_t)exp;
    int neg = 0;
    if (e < 0) {
        if (base == 0.0) {
            __ada_raise(__ada_exc_constraint_error);
        }
        neg = 1;
        e = -e;
    }
    while (e > 0) {
        if (e & 1) res *= b;
        e >>= 1;
        if (e > 0) b *= b;
    }
    return neg ? (1.0 / res) : res;
}

float __ada_fpow_f32(float base, int32_t exp) {
    return (float)__ada_fpow_f64((double)base, exp);
}

int64_t __ada_f64_to_i64_round(double x, int64_t lo, int64_t hi, int32_t chk) {
    if (isnan(x) || isinf(x)) {
        if (chk) __ada_raise(__ada_exc_constraint_error);
        return 0;
    }
    double r = round(x);
    if (r < -9223372036854775808.0 || r >= 9223372036854775808.0) {
        if (chk) __ada_raise(__ada_exc_constraint_error);
        return r < 0.0 ? INT64_MIN : INT64_MAX;
    }
    int64_t v = (int64_t)r;
    if (chk && (v < lo || v > hi)) {
        __ada_raise(__ada_exc_constraint_error);
    }
    return v;
}

#define ADA_HEAP_MAGIC 0xADA83A110C55AA11ULL

typedef struct AdaHeapHdr {
    uint64_t magic;
    uint64_t size;
} AdaHeapHdr;

_Static_assert(sizeof(AdaHeapHdr) % 16u == 0u, "AdaHeapHdr must preserve 16-byte alignment");

static size_t g_ada_heap_used  = 0u;
static size_t g_ada_heap_limit = 0u;
static pthread_once_t g_ada_heap_once = PTHREAD_ONCE_INIT;

static void ada_init_heap_limit_once(void) {
    uint64_t lim = (uint64_t)256u * 1024u * 1024u; /* 256 MiB default cap (cb1010b) */
    const char *env = getenv("ADA_HEAP_LIMIT_MB");
    if (env && env[0] != '\0') {
        char *endp = NULL;
        long mb = strtol(env, &endp, 10);
        if (endp != env && *endp == '\0' && mb >= 1 && mb <= 65536) {
            uint64_t req = (uint64_t)mb * 1024u * 1024u;
            if (req <= (uint64_t)SIZE_MAX) {
                lim = req;
            }
        }
    }
    __atomic_store_n(&g_ada_heap_limit, (size_t)lim, __ATOMIC_RELEASE);
}

static size_t ada_get_heap_limit(void) {
    pthread_once(&g_ada_heap_once, ada_init_heap_limit_once);
    return __atomic_load_n(&g_ada_heap_limit, __ATOMIC_ACQUIRE);
}

void *__ada_alloc(int64_t bytes) {
    if (bytes < 0) {
        __ada_raise(__ada_exc_storage_error);
    }
    size_t sz = (bytes > 0) ? (size_t)bytes : 1u;
    size_t lim = ada_get_heap_limit();
    if (sz > lim || sz > SIZE_MAX - sizeof(AdaHeapHdr)) {
        __ada_raise(__ada_exc_storage_error);
    }
    size_t cur = __atomic_load_n(&g_ada_heap_used, __ATOMIC_ACQUIRE);
    for (;;) {
        if (cur > lim || sz > lim - cur) {
            __ada_raise(__ada_exc_storage_error);
        }
        if (__atomic_compare_exchange_n(&g_ada_heap_used, &cur, cur + sz, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    AdaHeapHdr *hdr = (AdaHeapHdr *)calloc(1u, sizeof(AdaHeapHdr) + sz);
    if (!hdr) {
        __atomic_fetch_sub(&g_ada_heap_used, sz, __ATOMIC_ACQ_REL);
        __ada_raise(__ada_exc_storage_error);
    }
    hdr->size = (uint64_t)sz;
    __atomic_store_n(&hdr->magic, ADA_HEAP_MAGIC, __ATOMIC_RELEASE);
    return (void *)(hdr + 1);
}

static void ada_free_impl(int64_t *used_p, void *p) {
    if (!p) return;
    AdaHeapHdr *hdr = ((AdaHeapHdr *)p) - 1;
    if (__atomic_exchange_n(&hdr->magic, 0u, __ATOMIC_ACQ_REL) == ADA_HEAP_MAGIC) {
        size_t sz = (size_t)hdr->size;
        if (used_p) {
            __atomic_fetch_sub(used_p, (int64_t)sz, __ATOMIC_ACQ_REL);
        }
        __atomic_fetch_sub(&g_ada_heap_used, sz, __ATOMIC_ACQ_REL);
        free(hdr);
    }
}

void __ada_free(void *p) {
    ada_free_impl(NULL, p);
}

void *__ada_pool_alloc(int64_t *used_p, int64_t limit, int64_t bytes) {
    if (!used_p || limit <= 0 || bytes < 0) {
        __ada_raise(__ada_exc_storage_error);
    }
    int64_t req = (bytes > 0) ? bytes : 1;
    if (req > limit) {
        __ada_raise(__ada_exc_storage_error);
    }
    int64_t cur = __atomic_load_n(used_p, __ATOMIC_ACQUIRE);
    if (cur < 0 || cur > limit - req) {
        __ada_raise(__ada_exc_storage_error);
    }
    void *p = __ada_alloc(req);
    cur = __atomic_load_n(used_p, __ATOMIC_ACQUIRE);
    for (;;) {
        if (cur < 0 || cur > limit - req) {
            __ada_free(p);
            __ada_raise(__ada_exc_storage_error);
        }
        if (__atomic_compare_exchange_n(used_p, &cur, cur + req, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    return p;
}

void __ada_pool_free(int64_t *used_p, void *p) {
    ada_free_impl(used_p, p);
}

/* =========================================================================
 * Stack Overflow -> STORAGE_ERROR Signal Handler (cb1010a/c/d)
 * ========================================================================= */

static int g_sig_installed = 0;
static __thread uint64_t g_ada_emerg_stk[2048];
static __thread uintptr_t g_ada_stk_bot = 0u;
static __thread int g_ada_stk_known = 0;

void __ada_ss_destroy(void);

__attribute__((noreturn))
static void ada_raise_storage_error_trampoline(void) {
    __ada_raise(__ada_exc_storage_error);
}

static void ada_cache_thread_stack_bounds(void) {
    g_ada_stk_known = 0;
    g_ada_stk_bot = 0u;
#ifdef __APPLE__
    pthread_t self = pthread_self();
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    size_t    sz  = pthread_get_stacksize_np(self);
    if (top > sz && sz > 0u) {
        g_ada_stk_bot = top - sz;
        g_ada_stk_known = 1;
    }
#elif defined(__linux__)
    pthread_attr_t attr;
    void *stk_addr = NULL;
    size_t stk_sz  = 0u;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        int rc = pthread_attr_getstack(&attr, &stk_addr, &stk_sz);
        pthread_attr_destroy(&attr);
        if (rc == 0 && stk_addr != NULL && stk_sz > 0u) {
            g_ada_stk_bot = (uintptr_t)stk_addr;
            g_ada_stk_known = 1;
        }
    }
#endif
}

static void ada_stack_sig_handler(int sig, siginfo_t *si, void *uc) {
    if (g_ada_stk_known && si && si->si_addr != NULL && uc != NULL) {
        uintptr_t bot = g_ada_stk_bot;
        uintptr_t fa  = (uintptr_t)si->si_addr;
        uintptr_t lo  = (bot > 4194304u) ? (bot - 4194304u) : 65536u;
        if (lo < 65536u) lo = 65536u;
        uintptr_t hi  = bot + 65536u;
        uintptr_t em_lo = (uintptr_t)&g_ada_emerg_stk[0];
        uintptr_t em_hi = (uintptr_t)&g_ada_emerg_stk[2048];
        if (fa >= lo && fa <= hi) {
            ucontext_t *uctx = (ucontext_t *)uc;
            uintptr_t cur_sp = 0u;
            uintptr_t esp = em_hi & ~(uintptr_t)15u;
#if defined(__APPLE__) && defined(__aarch64__)
            cur_sp = (uintptr_t)uctx->uc_mcontext->__ss.__sp;
            if (cur_sp < em_lo || cur_sp > em_hi) {
                uctx->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)&ada_raise_storage_error_trampoline;
                uctx->uc_mcontext->__ss.__sp = (uint64_t)esp;
                uctx->uc_mcontext->__ss.__fp = (uint64_t)esp;
                uctx->uc_mcontext->__ss.__lr = 0u;
                return;
            }
#elif defined(__APPLE__) && defined(__x86_64__)
            cur_sp = (uintptr_t)uctx->uc_mcontext->__ss.__rsp;
            if (cur_sp < em_lo || cur_sp > em_hi) {
                uctx->uc_mcontext->__ss.__rip = (uint64_t)(uintptr_t)&ada_raise_storage_error_trampoline;
                uctx->uc_mcontext->__ss.__rsp = (uint64_t)(esp - 8u);
                uctx->uc_mcontext->__ss.__rbp = (uint64_t)esp;
                return;
            }
#elif defined(__linux__) && defined(__aarch64__)
            cur_sp = (uintptr_t)uctx->uc_mcontext.sp;
            if (cur_sp < em_lo || cur_sp > em_hi) {
                uctx->uc_mcontext.pc = (uint64_t)(uintptr_t)&ada_raise_storage_error_trampoline;
                uctx->uc_mcontext.sp = (uint64_t)esp;
                uctx->uc_mcontext.regs[29] = (uint64_t)esp;
                uctx->uc_mcontext.regs[30] = 0u;
                return;
            }
#elif defined(__linux__) && defined(__x86_64__)
            cur_sp = (uintptr_t)uctx->uc_mcontext.gregs[REG_RSP];
            if (cur_sp < em_lo || cur_sp > em_hi) {
                uctx->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)&ada_raise_storage_error_trampoline;
                uctx->uc_mcontext.gregs[REG_RSP] = (greg_t)(esp - 8u);
                uctx->uc_mcontext.gregs[REG_RBP] = (greg_t)esp;
                return;
            }
#else
            (void)uctx;
            (void)cur_sp;
            (void)esp;
            (void)em_lo;
#endif
        }
    }
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
    /* Returning re-executes the faulting instruction with SIG_DFL, preserving original PC. */
}

void *__ada_install_stack_overflow_handler(void) {
    ada_cache_thread_stack_bounds();
    size_t alt_sz = 65536u;
#ifdef SIGSTKSZ
    long sys_alt = (long)SIGSTKSZ;
    if (sys_alt > 0 && (size_t)sys_alt > alt_sz) alt_sz = (size_t)sys_alt;
#endif
    void *alt_buf = malloc(alt_sz);
    if (alt_buf) {
        stack_t ss;
        ss.ss_sp    = alt_buf;
        ss.ss_size  = alt_sz;
        ss.ss_flags = 0;
        if (sigaltstack(&ss, NULL) != 0) {
            free(alt_buf);
            alt_buf = NULL;
        }
    }
    int expected = 0;
    if (__atomic_compare_exchange_n(&g_sig_installed, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = ada_stack_sig_handler;
        sa.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
    }
    return alt_buf;
}

void __ada_uninstall_stack_overflow_handler(void *alt_buf) {
    __ada_ss_destroy();
    g_ada_stk_known = 0;
    if (alt_buf) {
        stack_t ss;
        memset(&ss, 0, sizeof(ss));
        ss.ss_flags = SS_DISABLE;
        (void)sigaltstack(&ss, NULL);
        free(alt_buf);
    }
}

__attribute__((constructor))
static void ada_rt_init_stack_handler(void) {
    (void)__ada_install_stack_overflow_handler();
}

/* =========================================================================
 * Ada Secondary Stack (for unconstrained / dynamic array function returns
 * and `&` concatenation temporaries).
 * ========================================================================= */

typedef struct AdaSSChunk {
    struct AdaSSChunk *prev;
    struct AdaSSChunk *next;
    uint64_t           base_offset;
    size_t             cap;
    size_t             used;
    uint64_t           _pad; /* ensures 48-byte header -> 16-byte aligned data[] */
    uint8_t            data[];
} AdaSSChunk;

_Static_assert(sizeof(AdaSSChunk) % 16u == 0u, "AdaSSChunk.data must be 16-byte aligned");

static __thread AdaSSChunk *g_ada_ss_cur = NULL;
static pthread_key_t g_ada_ss_key;
static pthread_once_t g_ada_ss_key_once = PTHREAD_ONCE_INIT;

static void ada_ss_free_chain(AdaSSChunk *c) {
    if (!c) return;
    while (c->prev) c = c->prev;
    while (c) {
        AdaSSChunk *nx = c->next;
        free(c);
        c = nx;
    }
}

static void ada_ss_tls_destructor(void *val) {
    AdaSSChunk *c = (AdaSSChunk *)val;
    if (c == g_ada_ss_cur) {
        g_ada_ss_cur = NULL;
    }
    ada_ss_free_chain(c);
}

static void ada_ss_init_key(void) {
    (void)pthread_key_create(&g_ada_ss_key, ada_ss_tls_destructor);
}

void __ada_ss_destroy(void) {
    AdaSSChunk *c = g_ada_ss_cur;
    g_ada_ss_cur = NULL;
    pthread_once(&g_ada_ss_key_once, ada_ss_init_key);
    (void)pthread_setspecific(g_ada_ss_key, NULL);
    ada_ss_free_chain(c);
}

static AdaSSChunk *ada_ss_new_chunk(size_t cap, uint64_t base_offset) {
    if (cap < 65536u) cap = 65536u;
    if (cap > SIZE_MAX - sizeof(AdaSSChunk)) {
        __ada_raise(__ada_exc_storage_error);
    }
    AdaSSChunk *c = (AdaSSChunk *)malloc(sizeof(AdaSSChunk) + cap);
    if (!c) {
        __ada_raise(__ada_exc_storage_error);
    }
    c->prev        = NULL;
    c->next        = NULL;
    c->base_offset = base_offset;
    c->cap         = cap;
    c->used        = 0u;
    c->_pad        = 0u;
    return c;
}

uint64_t __ada_ss_mark(void) {
    if (!g_ada_ss_cur) return 0u;
    return g_ada_ss_cur->base_offset + (uint64_t)g_ada_ss_cur->used;
}

static int ada_ss_try_alloc_in_chunk(AdaSSChunk *c, size_t sz, size_t al, void **out_ptr) {
    uintptr_t base_addr = (uintptr_t)c->data;
    if (c->used > c->cap) return 0;
    uintptr_t cur_addr = base_addr + c->used;
    size_t mis = (size_t)(cur_addr & (al - 1u));
    size_t pad = (mis == 0u) ? 0u : (al - mis);
    if (pad > c->cap - c->used) return 0;
    size_t aligned = c->used + pad;
    if (sz > c->cap - aligned) return 0;
    *out_ptr = &c->data[aligned];
    c->used = aligned + sz;
    return 1;
}

void *__ada_ss_alloc(int64_t bytes, int64_t align) {
    if (bytes < 0) {
        __ada_raise(__ada_exc_storage_error);
    }
    size_t sz = (bytes > 0) ? (size_t)bytes : 1u;
    size_t al = (align > 1) ? (size_t)align : 1u;
    if ((al & (al - 1u)) != 0u || al > 4096u) al = 16u;
    if (al < 8u) al = 8u;
    if (sz > SIZE_MAX - al) {
        __ada_raise(__ada_exc_storage_error);
    }

    if (!g_ada_ss_cur) {
        size_t need = sz + al;
        size_t init_cap = (need > 65536u) ? need : 65536u;
        g_ada_ss_cur = ada_ss_new_chunk(init_cap, 0u);
        pthread_once(&g_ada_ss_key_once, ada_ss_init_key);
        (void)pthread_setspecific(g_ada_ss_key, g_ada_ss_cur);
    }

    void *p = NULL;
    if (ada_ss_try_alloc_in_chunk(g_ada_ss_cur, sz, al, &p)) {
        return p;
    }

    /* Fast reuse of cached next chunk if large enough. */
    if (g_ada_ss_cur->next) {
        g_ada_ss_cur->next->used = 0u;
        if (ada_ss_try_alloc_in_chunk(g_ada_ss_cur->next, sz, al, &p)) {
            g_ada_ss_cur = g_ada_ss_cur->next;
            (void)pthread_setspecific(g_ada_ss_key, g_ada_ss_cur);
            return p;
        }
    }

    /* Free any too-small cached tail chunks above current top. */
    AdaSSChunk *tail = g_ada_ss_cur->next;
    while (tail) {
        AdaSSChunk *nx = tail->next;
        free(tail);
        tail = nx;
    }
    g_ada_ss_cur->next = NULL;

    size_t need = sz + al;
    size_t next_cap = (g_ada_ss_cur->cap <= SIZE_MAX / 2u) ? (g_ada_ss_cur->cap * 2u) : SIZE_MAX;
    if (next_cap < need) next_cap = need;
    uint64_t next_base = g_ada_ss_cur->base_offset + (uint64_t)g_ada_ss_cur->cap;
    AdaSSChunk *nc = ada_ss_new_chunk(next_cap, next_base);
    nc->prev = g_ada_ss_cur;
    g_ada_ss_cur->next = nc;
    g_ada_ss_cur = nc;
    (void)pthread_setspecific(g_ada_ss_key, g_ada_ss_cur);
    if (!ada_ss_try_alloc_in_chunk(nc, sz, al, &p)) {
        __ada_raise(__ada_exc_storage_error);
    }
    return p;
}

void __ada_ss_release(uint64_t mark) {
    if (!g_ada_ss_cur) return;
    while (g_ada_ss_cur->prev != NULL && g_ada_ss_cur->base_offset > mark) {
        g_ada_ss_cur->used = 0u;
        g_ada_ss_cur = g_ada_ss_cur->prev;
    }
    if (mark >= g_ada_ss_cur->base_offset) {
        size_t off = (size_t)(mark - g_ada_ss_cur->base_offset);
        g_ada_ss_cur->used = (off <= g_ada_ss_cur->cap) ? off : g_ada_ss_cur->cap;
    } else {
        g_ada_ss_cur->used = 0u;
    }
    /* Keep at most one small (<= 1 MiB) cached tail chunk and reclaim everything beyond it */
    AdaSSChunk *keep = g_ada_ss_cur;
    if (keep->next && keep->next->cap <= (size_t)1024u * 1024u) {
        keep = keep->next;
    }
    AdaSSChunk *tail = keep->next;
    keep->next = NULL;
    while (tail) {
        AdaSSChunk *nx = tail->next;
        free(tail);
        tail = nx;
    }
    (void)pthread_setspecific(g_ada_ss_key, g_ada_ss_cur);
}

/* =========================================================================
 * Dynamic Discrete 'WIDTH & Fixed-Point 'MANTISSA / 'LARGE / 'FORE Helpers
 * ========================================================================= */

int32_t __ada_int_width(int64_t lo, int64_t hi) {
    if (hi < lo) return 0;
    char b1[32], b2[32];
    int n1 = (lo >= 0) ? snprintf(b1, sizeof(b1), " %" PRId64, lo)
                       : snprintf(b1, sizeof(b1), "%" PRId64, lo);
    int n2 = (hi >= 0) ? snprintf(b2, sizeof(b2), " %" PRId64, hi)
                       : snprintf(b2, sizeof(b2), "%" PRId64, hi);
    return (n1 > n2) ? (int32_t)n1 : (int32_t)n2;
}

int32_t __ada_enum_width(const char *const *tbl, int32_t cnt, int64_t lo, int64_t hi) {
    if (hi < lo || cnt <= 0 || !tbl) return 0;
    int64_t s = (lo < 0) ? 0 : lo;
    int64_t e = (hi >= (int64_t)cnt) ? ((int64_t)cnt - 1) : hi;
    int32_t w = 0;
    for (int64_t i = s; i <= e; i++) {
        int32_t len = (int32_t)strlen(tbl[i]);
        if (len > w) w = len;
    }
    return w;
}

static uint64_t ada_abs_i64(int64_t v) {
    if (v >= 0) return (uint64_t)v;
    return (uint64_t)(-(v + 1)) + 1ULL;
}

static uint64_t ada_gcd_u64(uint64_t a, uint64_t b) {
    while (b != 0ULL) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

int32_t __ada_fixed_mantissa(int64_t lo_ticks, int64_t hi_ticks, double base_small, double sub_small) {
    if (hi_ticks < lo_ticks) return 0;
    uint64_t alo = ada_abs_i64(lo_ticks);
    uint64_t ahi = ada_abs_i64(hi_ticks);
    uint64_t mt = (alo > ahi) ? alo : ahi;
    if (mt == 0ULL) return 0;
    if (!(base_small > 0.0) || !(sub_small > 0.0)) return 1;
    if (sub_small >= base_small) {
        double div = sub_small / base_small;
        int exp = 0;
        double m = frexp(div, &exp);
        if (m == 0.5 && exp >= 1 && exp <= 63) {
            int k = exp - 1; /* div == 2^k */
            int p = (mt <= 1ULL) ? 0 : (64 - __builtin_clzll(mt - 1ULL));
            int b = p - k;
            if (b < 1) b = 1;
            if (b > 63) b = 63;
            return (int32_t)b;
        }
    }
    double r = (double)mt * (base_small / sub_small);
    if (!(r > 0.0)) return 1;
    int b = 1;
    double p2 = 2.0;
    while (b < 63 && p2 < r) {
        p2 *= 2.0;
        b++;
    }
    return (int32_t)b;
}

int64_t __ada_fixed_large_ticks(int64_t lo_ticks, int64_t hi_ticks, double base_small, double sub_small) {
    int32_t b = __ada_fixed_mantissa(lo_ticks, hi_ticks, base_small, sub_small);
    if (b <= 0) return 0;
    uint64_t mag = (b >= 63) ? 0x7FFFFFFFFFFFFFFFULL : ((1ULL << b) - 1ULL);
    if (!(base_small > 0.0) || !(sub_small > 0.0)) return (int64_t)mag;
    if (sub_small >= base_small) {
        double div = sub_small / base_small;
        int exp = 0;
        double m = frexp(div, &exp);
        if (m == 0.5 && exp >= 1 && exp <= 63) {
            int k = exp - 1;
            if (k >= 63 || mag > (0x7FFFFFFFFFFFFFFFULL >> k)) {
                return INT64_MAX;
            }
            return (int64_t)(mag << k);
        }
    }
    double v = (double)mag * (sub_small / base_small);
    if (!(v >= 0.0)) return 0;
    if (!(v < 9223372036854775808.0)) return INT64_MAX;
    double r = v + 0.5;
    if (!(r < 9223372036854775808.0)) return INT64_MAX;
    return (int64_t)r;
}

int32_t __ada_fixed_fore(int64_t lo_ticks, int64_t hi_ticks, double base_small) {
    if (hi_ticks < lo_ticks) return 2;
    uint64_t alo = ada_abs_i64(lo_ticks);
    uint64_t ahi = ada_abs_i64(hi_ticks);
    uint64_t mt = (alo > ahi) ? alo : ahi;
    double m = (double)mt * base_small;
    int32_t n = 2;
    double p10 = 10.0;
    while (n < 39 && m >= p10) {
        n++;
        p10 *= 10.0;
    }
    return n;
}

int32_t __ada_fixed_fore_rat(int64_t lo_ticks, int64_t hi_ticks, int64_t sm_num, int64_t sm_den) {
    if (hi_ticks < lo_ticks) return 2;
    if (sm_num <= 0 || sm_den <= 0) {
        return __ada_fixed_fore(lo_ticks, hi_ticks, 1.0);
    }
    unsigned __int128 alo = (unsigned __int128)ada_abs_i64(lo_ticks);
    unsigned __int128 ahi = (unsigned __int128)ada_abs_i64(hi_ticks);
    unsigned __int128 mt = (alo > ahi) ? alo : ahi;
    unsigned __int128 prod = mt * (unsigned __int128)sm_num;
    unsigned __int128 thresh = (unsigned __int128)sm_den * 10u;
    int32_t n = 2;
    while (n < 39 && prod >= thresh) {
        n++;
        if (thresh > (((unsigned __int128)0 - (unsigned __int128)1) / 10u)) break;
        thresh *= 10u;
    }
    return n;
}

static int64_t ada_apply_signed_q(unsigned __int128 q, int neg, int64_t lo, int64_t hi, int32_t chk) {
    if (!neg) {
        if (q > (unsigned __int128)INT64_MAX) {
            if (chk) __ada_raise(__ada_exc_constraint_error);
            return INT64_MAX;
        }
        int64_t v = (int64_t)q;
        if (chk && (v < lo || v > hi)) __ada_raise(__ada_exc_constraint_error);
        return v;
    } else {
        unsigned __int128 max_neg = (unsigned __int128)INT64_MAX + 1u;
        if (q > max_neg) {
            if (chk) __ada_raise(__ada_exc_constraint_error);
            return INT64_MIN;
        }
        int64_t v = (q == max_neg) ? INT64_MIN : -(int64_t)q;
        if (chk && (v < lo || v > hi)) __ada_raise(__ada_exc_constraint_error);
        return v;
    }
}

static int64_t ada_i128_div_round(__int128 num, __int128 den, int64_t lo, int64_t hi, int32_t chk) {
    if (den == 0) {
        if (chk) __ada_raise(__ada_exc_constraint_error);
        return 0;
    }
    int neg = 0;
    unsigned __int128 un, ud;
    if (num < 0) { neg = !neg; un = (unsigned __int128)(-(num + 1)) + 1u; }
    else { un = (unsigned __int128)num; }
    if (den < 0) { neg = !neg; ud = (unsigned __int128)(-(den + 1)) + 1u; }
    else { ud = (unsigned __int128)den; }
    unsigned __int128 q = un / ud;
    unsigned __int128 r = un % ud;
    if (r * 2u >= ud) q += 1u; /* round halfway away from zero */
    return ada_apply_signed_q(q, neg, lo, hi, chk);
}

int64_t __ada_fixed_mul_i64(int64_t a, int64_t b, int64_t snum, int64_t sden,
                            int64_t lo, int64_t hi, int32_t chk) {
    if (sden == 0) {
        if (chk) __ada_raise(__ada_exc_constraint_error);
        return 0;
    }
    if (a == 0 || b == 0 || snum == 0) {
        if (chk && (0 < lo || 0 > hi)) __ada_raise(__ada_exc_constraint_error);
        return 0;
    }
    int neg = (a < 0) ^ (b < 0) ^ (snum < 0) ^ (sden < 0);
    uint64_t ua = ada_abs_i64(a);
    uint64_t ub = ada_abs_i64(b);
    uint64_t usn = ada_abs_i64(snum);
    uint64_t ud = ada_abs_i64(sden);

    /* Exact GCD reduction before 192-bit / 64-bit exact division (zero double precision loss). */
    uint64_t g = ada_gcd_u64(usn, ud);
    usn /= g; ud /= g;
    if (ud > 1ULL) { g = ada_gcd_u64(ua, ud); ua /= g; ud /= g; }
    if (ud > 1ULL) { g = ada_gcd_u64(ub, ud); ub /= g; ud /= g; }

    unsigned __int128 p01 = (unsigned __int128)ua * (unsigned __int128)ub;
    uint64_t p_lo = (uint64_t)p01;
    uint64_t p_hi = (uint64_t)(p01 >> 64);

    unsigned __int128 t0 = (unsigned __int128)p_lo * (unsigned __int128)usn;
    uint64_t w0 = (uint64_t)t0;
    uint64_t c0 = (uint64_t)(t0 >> 64);
    unsigned __int128 t1 = (unsigned __int128)p_hi * (unsigned __int128)usn + (unsigned __int128)c0;
    uint64_t w1 = (uint64_t)t1;
    uint64_t w2 = (uint64_t)(t1 >> 64);

    uint64_t q2 = w2 / ud;
    uint64_t r2 = w2 % ud;
    if (q2 > 0ULL) {
        if (chk) __ada_raise(__ada_exc_constraint_error);
        return neg ? INT64_MIN : INT64_MAX;
    }
    unsigned __int128 d1 = ((unsigned __int128)r2 << 64) | (unsigned __int128)w1;
    uint64_t q1 = (uint64_t)(d1 / (unsigned __int128)ud);
    uint64_t r1 = (uint64_t)(d1 % (unsigned __int128)ud);
    if (q1 > 0ULL) {
        if (chk) __ada_raise(__ada_exc_constraint_error);
        return neg ? INT64_MIN : INT64_MAX;
    }
    unsigned __int128 d0 = ((unsigned __int128)r1 << 64) | (unsigned __int128)w0;
    unsigned __int128 q = d0 / (unsigned __int128)ud;
    unsigned __int128 r = d0 % (unsigned __int128)ud;
    if (r * 2u >= (unsigned __int128)ud) q += 1u;
    return ada_apply_signed_q(q, neg, lo, hi, chk);
}

int64_t __ada_fixed_div_i64(int64_t a, int64_t b, int64_t snum, int64_t sden,
                            int64_t lo, int64_t hi, int32_t chk) {
    if (b == 0 || sden == 0) {
        if (chk) __ada_raise(__ada_exc_constraint_error);
        return 0;
    }
    if (a == 0 || snum == 0) {
        if (chk && (0 < lo || 0 > hi)) __ada_raise(__ada_exc_constraint_error);
        return 0;
    }
    return ada_i128_div_round((__int128)a * (__int128)snum, (__int128)b * (__int128)sden, lo, hi, chk);
}
