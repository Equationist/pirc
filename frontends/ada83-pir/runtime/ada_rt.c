/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Minimal C99 Ada 83 Runtime for diana2pir + pirc.
 * Provides:
 *   - Standard Ada exception tags & unhandled exception handler
 *   - Text_IO (Character, String, Integer_IO, Float_IO)
 *   - Calendar (Clock in nanoseconds via CLOCK_MONOTONIC)
 *   - Integer/Float exponentiation helpers with optional overflow checks
 */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#include <sys/ucontext.h>
#endif
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
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

void __ada_raise(const void *exc_tag) {
    __pir_unwind_resume((void *)exc_tag);
    abort();
}

void __ada_unhandled_exception(const void *exc_tag) {
    const char *name = exc_tag ? (const char *)exc_tag : "UNKNOWN_EXCEPTION";
    fprintf(stderr, "raised %s\n", name);
    fflush(stderr);
    exit(1);
}

/* =========================================================================
 * Text_IO Runtime Support
 * ========================================================================= */

static int32_t g_stdout_col = 1;

void __ada_put_char(int8_t c) {
    unsigned char uc = (unsigned char)c;
    putchar(uc);
    if (uc == '\n') {
        g_stdout_col = 1;
    } else {
        g_stdout_col++;
    }
}

void __ada_put_str(const char *ptr, int32_t len) {
    if (len > 0 && ptr != NULL) {
        fwrite(ptr, 1u, (size_t)len, stdout);
        for (int32_t i = 0; i < len; ++i) {
            if (ptr[i] == '\n') g_stdout_col = 1;
            else g_stdout_col++;
        }
    }
}

void __ada_put_line(const char *ptr, int32_t len) {
    if (len > 0 && ptr != NULL) {
        fwrite(ptr, 1u, (size_t)len, stdout);
    }
    putchar('\n');
    g_stdout_col = 1;
}

void __ada_new_line(int32_t spacing) {
    for (int32_t i = 0; i < spacing; ++i) {
        putchar('\n');
    }
    if (spacing > 0) {
        g_stdout_col = 1;
    }
}

void __ada_set_col(int32_t to) {
    if (to < 1) to = 1;
    if (g_stdout_col > to) {
        putchar('\n');
        g_stdout_col = 1;
    }
    while (g_stdout_col < to) {
        putchar(' ');
        g_stdout_col++;
    }
}

void __ada_put_i64(int64_t val, int32_t width, int32_t base) {
    char buf[128];
    if (base == 10 || base < 2 || base > 16) {
        snprintf(buf, sizeof(buf), "%" PRId64, val);
    } else {
        /* Ada based literal format: BASE#DIGITS# */
        static const char digits[] = "0123456789ABCDEF";
        char rev[96];
        int neg = 0;
        uint64_t u = (uint64_t)val;
        if (val < 0) {
            neg = 1;
            u = (uint64_t)(-(val + 1)) + 1u;
        }
        int rlen = 0;
        if (u == 0) {
            rev[rlen++] = '0';
        } else {
            while (u > 0) {
                rev[rlen++] = digits[u % (uint64_t)base];
                u /= (uint64_t)base;
            }
        }
        int pos = snprintf(buf, sizeof(buf), "%s%d#", neg ? "-" : "", (int)base);
        for (int i = rlen - 1; i >= 0 && pos < (int)sizeof(buf) - 2; --i) {
            buf[pos++] = rev[i];
        }
        buf[pos++] = '#';
        buf[pos] = '\0';
    }
    int len = (int)strlen(buf);
    for (int i = len; i < (int)width; ++i) {
        putchar(' ');
    }
    fputs(buf, stdout);
}

void __ada_put_f64(double val, int32_t fore, int32_t aft, int32_t exp_digits) {
    if (aft < 1) aft = 1;
    char buf[128];
    if (exp_digits <= 0) {
        snprintf(buf, sizeof(buf), "%.*f", (int)aft, val);
    } else {
        snprintf(buf, sizeof(buf), "%.*E", (int)aft, val);
    }
    /* Pad fore (characters before '.') */
    const char *dot = strchr(buf, '.');
    int before_dot = dot ? (int)(dot - buf) : (int)strlen(buf);
    for (int i = before_dot; i < (int)fore; ++i) {
        putchar(' ');
    }
    fputs(buf, stdout);
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
 * 69 years including 17 leap years (1904..1968) = 25202 days = 2177452800 s
 * STANDARD.DURATION has small = 2^(-30) s, so 1 second = (1LL << 30) ticks.
 * 199 years (72683 days) * 86400 * 2^30 = 6.74e18 < INT64_MAX (9.22e18). */
#define ADA_EPOCH_OFFSET_SECS 2177452800LL
#define ADA_TICKS_PER_SEC     1000000000LL
#define ADA_TICKS_PER_DAY     (86400LL * ADA_TICKS_PER_SEC)
#define ADA_TOTAL_DAYS_2099   72684LL
#define ADA_MAX_TIME_TICKS    (ADA_TOTAL_DAYS_2099 * ADA_TICKS_PER_DAY)

int64_t __ada_clock_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t sec_ticks = ((int64_t)ts.tv_sec + ADA_EPOCH_OFFSET_SECS) * ADA_TICKS_PER_SEC;
    int64_t sub_ticks = (int64_t)ts.tv_nsec;
    return sec_ticks + sub_ticks;
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
    /* Use __int128 or safe bounds check to avoid signed overflow */
    if (d > 0 && t > ADA_MAX_TIME_TICKS - d) {
        __ada_raise(__ada_exc_time_error);
    }
    if (d < 0 && t < -d) {
        __ada_raise(__ada_exc_time_error);
    }
    int64_t res = t + d;
    if (res < 0 || res >= ADA_MAX_TIME_TICKS) {
        __ada_raise(__ada_exc_time_error);
    }
    return res;
}

int64_t __ada_cal_sub_dur(int64_t t, int64_t d) {
    if (d < 0 && t > ADA_MAX_TIME_TICKS + d) {
        __ada_raise(__ada_exc_time_error);
    }
    if (d > 0 && t < d) {
        __ada_raise(__ada_exc_time_error);
    }
    int64_t res = t - d;
    if (res < 0 || res >= ADA_MAX_TIME_TICKS) {
        __ada_raise(__ada_exc_time_error);
    }
    return res;
}

int64_t __ada_cal_sub_time(int64_t t1, int64_t t2) {
    /* Result must fit within STANDARD.DURATION (-86400.0 .. +86400.0) */
    int64_t diff = t1 - t2;
    if (diff < -ADA_TICKS_PER_DAY || diff > ADA_TICKS_PER_DAY) {
        __ada_raise(__ada_exc_time_error);
    }
    return diff;
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
    if (val >= 0) {
        return (int32_t)snprintf(buf, 32, " %" PRId64, val);
    }
    return (int32_t)snprintf(buf, 32, "%" PRId64, val);
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
    return 0;
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
    return 0;
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
                expv = expv * 10 + (ptr[i] - '0');
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
                res *= b;
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
                b *= b;
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

int64_t __ada_f64_to_i64_trunc(double x, int64_t lo, int64_t hi, int32_t chk) {
    if (isnan(x) || isinf(x)) {
        if (chk) __ada_raise(__ada_exc_constraint_error);
        return 0;
    }
    double r = trunc(x);
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
    size_t   size;
} AdaHeapHdr;

static volatile size_t g_ada_heap_used  = 0u;
static volatile size_t g_ada_heap_limit = 0u;

static size_t ada_get_heap_limit(void) {
    size_t lim = g_ada_heap_limit;
    if (lim == 0u) {
        lim = (size_t)256u * 1024u * 1024u; /* 256 MiB default cap (cb1010b) */
        const char *env = getenv("ADA_HEAP_LIMIT_MB");
        if (env && env[0] != '\0') {
            long mb = strtol(env, NULL, 10);
            if (mb >= 1 && mb <= 65536) {
                lim = (size_t)mb * 1024u * 1024u;
            }
        }
        g_ada_heap_limit = lim;
    }
    return lim;
}

void *__ada_alloc(int64_t bytes) {
    if (bytes <= 0) bytes = 1;
    size_t sz = (size_t)bytes;
    size_t lim = ada_get_heap_limit();
    if (sz > lim) {
        __ada_raise(__ada_exc_storage_error);
    }
    size_t prev = __sync_fetch_and_add(&g_ada_heap_used, sz);
    if (prev + sz > lim || prev + sz < prev) {
        __sync_fetch_and_sub(&g_ada_heap_used, sz);
        __ada_raise(__ada_exc_storage_error);
    }
    AdaHeapHdr *hdr = (AdaHeapHdr *)calloc(1u, sizeof(AdaHeapHdr) + sz);
    if (!hdr) {
        __sync_fetch_and_sub(&g_ada_heap_used, sz);
        __ada_raise(__ada_exc_storage_error);
    }
    hdr->magic = ADA_HEAP_MAGIC;
    hdr->size  = sz;
    return (void *)(hdr + 1);
}

void __ada_free(void *p) {
    if (!p) return;
    AdaHeapHdr *hdr = ((AdaHeapHdr *)p) - 1;
    if (hdr->magic == ADA_HEAP_MAGIC) {
        size_t sz = hdr->size;
        hdr->magic = 0u;
        __sync_fetch_and_sub(&g_ada_heap_used, sz);
        free(hdr);
    } else {
        free(p);
    }
}

void *__ada_pool_alloc(int64_t *used_p, int64_t limit, int64_t bytes) {
    if (bytes <= 0) bytes = 1;
    if (limit <= 0) {
        __ada_raise(__ada_exc_storage_error);
    }
    int64_t prev = __sync_fetch_and_add(used_p, bytes);
    if (prev + bytes > limit || prev + bytes < prev) {
        __sync_fetch_and_sub(used_p, bytes);
        __ada_raise(__ada_exc_storage_error);
    }
    void *p = __ada_alloc(bytes);
    return p;
}

void __ada_pool_free(int64_t *used_p, void *p) {
    if (!p) return;
    AdaHeapHdr *hdr = ((AdaHeapHdr *)p) - 1;
    if (hdr->magic == ADA_HEAP_MAGIC && used_p) {
        int64_t sz = (int64_t)hdr->size;
        __sync_fetch_and_sub(used_p, sz);
    }
    __ada_free(p);
}

/* =========================================================================
 * Stack Overflow -> STORAGE_ERROR Signal Handler (cb1010a/c/d)
 * ========================================================================= */

static volatile int g_sig_installed = 0;
static __thread uint64_t g_ada_emerg_stk[2048];

static void ada_raise_storage_error_trampoline(void) {
    __ada_raise(__ada_exc_storage_error);
}

static void ada_stack_sig_handler(int sig, siginfo_t *si, void *uc) {
#ifdef __APPLE__
    if (si && si->si_addr != NULL && uc != NULL) {
        pthread_t self = pthread_self();
        uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
        size_t    sz  = pthread_get_stacksize_np(self);
        uintptr_t bot = (top > sz) ? (top - sz) : 0u;
        uintptr_t fa  = (uintptr_t)si->si_addr;
        uintptr_t lo  = (bot > 262144u) ? (bot - 262144u) : 0u;
        uintptr_t hi  = bot + 65536u;
        if (fa >= lo && fa <= hi) {
            ucontext_t *uctx = (ucontext_t *)uc;
            uintptr_t esp = ((uintptr_t)&g_ada_emerg_stk[2048]) & ~(uintptr_t)15u;
#if defined(__aarch64__)
            uctx->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)&ada_raise_storage_error_trampoline;
            uctx->uc_mcontext->__ss.__sp = (uint64_t)esp;
            uctx->uc_mcontext->__ss.__fp = (uint64_t)esp;
            uctx->uc_mcontext->__ss.__lr = 0u;
            return;
#elif defined(__x86_64__)
            uctx->uc_mcontext->__ss.__rip = (uint64_t)(uintptr_t)&ada_raise_storage_error_trampoline;
            uctx->uc_mcontext->__ss.__rsp = (uint64_t)(esp - 8u);
            uctx->uc_mcontext->__ss.__rbp = (uint64_t)esp;
            return;
#else
            __ada_raise(__ada_exc_storage_error);
#endif
        }
    }
#else
    (void)si;
    (void)uc;
#endif
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

void *__ada_install_stack_overflow_handler(void) {
    size_t alt_sz = 65536u;
#ifdef SIGSTKSZ
    if ((size_t)SIGSTKSZ > alt_sz) alt_sz = (size_t)SIGSTKSZ;
#endif
    void *alt_buf = malloc(alt_sz);
    if (alt_buf) {
        stack_t ss;
        ss.ss_sp    = alt_buf;
        ss.ss_size  = alt_sz;
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }
    if (__sync_bool_compare_and_swap(&g_sig_installed, 0, 1)) {
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
    if (alt_buf) {
        stack_t ss;
        memset(&ss, 0, sizeof(ss));
        ss.ss_flags = SS_DISABLE;
        sigaltstack(&ss, NULL);
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
 *
 * Uses a linked list of pointer-stable chunks with cached reuse:
 *   - O(1) mark, O(1) bump-pointer alloc, O(1) release (zero malloc/free
 *     churn in loops)
 *   - Pointer stability: growing allocates a new linked chunk without
 *     moving previously returned arrays in earlier chunks
 *   - Exception safe: restoring a saved mark reclaims all unwound frames
 * ========================================================================= */

typedef struct AdaSSChunk {
    struct AdaSSChunk *prev;
    struct AdaSSChunk *next;
    uint64_t           base_offset;
    size_t             cap;
    size_t             used;
    uint8_t            data[];
} AdaSSChunk;

static __thread AdaSSChunk *g_ada_ss_cur = NULL;

static AdaSSChunk *ada_ss_new_chunk(size_t cap, uint64_t base_offset) {
    if (cap < 65536u) cap = 65536u;
    AdaSSChunk *c = (AdaSSChunk *)malloc(sizeof(AdaSSChunk) + cap);
    if (!c) {
        __ada_raise(__ada_exc_storage_error);
    }
    c->prev        = NULL;
    c->next        = NULL;
    c->base_offset = base_offset;
    c->cap         = cap;
    c->used        = 0u;
    return c;
}

uint64_t __ada_ss_mark(void) {
    if (!g_ada_ss_cur) return 0u;
    return g_ada_ss_cur->base_offset + (uint64_t)g_ada_ss_cur->used;
}

void *__ada_ss_alloc(int64_t bytes, int64_t align) {
    size_t sz = (bytes > 0) ? (size_t)bytes : 1u;
    size_t al = (align > 1) ? (size_t)align : 1u;
    if ((al & (al - 1u)) != 0u) al = 16u;
    if (al < 8u) al = 8u;

    if (!g_ada_ss_cur) {
        size_t init_cap = (sz + al > 65536u) ? (sz + al) : 65536u;
        g_ada_ss_cur = ada_ss_new_chunk(init_cap, 0u);
    }

    size_t aligned = (g_ada_ss_cur->used + (al - 1u)) & ~(al - 1u);
    if (aligned + sz <= g_ada_ss_cur->cap) {
        void *p = &g_ada_ss_cur->data[aligned];
        g_ada_ss_cur->used = aligned + sz;
        return p;
    }

    /* Fast reuse of cached next chunk if large enough. */
    if (g_ada_ss_cur->next && g_ada_ss_cur->next->cap >= sz) {
        g_ada_ss_cur = g_ada_ss_cur->next;
        g_ada_ss_cur->used = sz;
        return g_ada_ss_cur->data;
    }

    /* Free any too-small cached tail chunks above current top. */
    AdaSSChunk *tail = g_ada_ss_cur->next;
    while (tail) {
        AdaSSChunk *nx = tail->next;
        free(tail);
        tail = nx;
    }
    g_ada_ss_cur->next = NULL;

    size_t next_cap = g_ada_ss_cur->cap * 2u;
    if (next_cap < sz) next_cap = sz;
    uint64_t next_base = g_ada_ss_cur->base_offset + (uint64_t)g_ada_ss_cur->cap;
    AdaSSChunk *nc = ada_ss_new_chunk(next_cap, next_base);
    nc->prev = g_ada_ss_cur;
    g_ada_ss_cur->next = nc;
    g_ada_ss_cur = nc;
    g_ada_ss_cur->used = sz;
    return g_ada_ss_cur->data;
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
}

/* =========================================================================
 * Dynamic Discrete 'WIDTH & Fixed-Point 'MANTISSA / 'LARGE / 'FORE Helpers
 * ========================================================================= */

int32_t __ada_int_width(int64_t lo, int64_t hi) {
    if (hi < lo) return 0;
    char b1[32], b2[32];
    int n1 = (lo >= 0) ? snprintf(b1, sizeof(b1), " %lld", (long long)lo)
                       : snprintf(b1, sizeof(b1), "%lld", (long long)lo);
    int n2 = (hi >= 0) ? snprintf(b2, sizeof(b2), " %lld", (long long)hi)
                       : snprintf(b2, sizeof(b2), "%lld", (long long)hi);
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

int32_t __ada_fixed_mantissa(int64_t lo_ticks, int64_t hi_ticks, double base_small, double sub_small) {
    if (hi_ticks < lo_ticks) return 0;
    uint64_t alo = ada_abs_i64(lo_ticks);
    uint64_t ahi = ada_abs_i64(hi_ticks);
    uint64_t mt = (alo > ahi) ? alo : ahi;
    if (mt == 0ULL) return 0;
    if (base_small > 0.0 && sub_small >= base_small) {
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
    if (base_small > 0.0 && sub_small >= base_small) {
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
    if (v >= (double)INT64_MAX) return INT64_MAX;
    return (int64_t)(v + 0.5);
}

int32_t __ada_fixed_fore(int64_t lo_ticks, int64_t hi_ticks, double base_small) {
    if (hi_ticks < lo_ticks) return 2;
    uint64_t alo = ada_abs_i64(lo_ticks);
    uint64_t ahi = ada_abs_i64(hi_ticks);
    uint64_t mt = (alo > ahi) ? alo : ahi;
    double m = (double)mt * base_small;
    int32_t n = 2;
    double p10 = 10.0;
    while (n < 38 && m >= p10) {
        n++;
        p10 *= 10.0;
    }
    return n;
}
