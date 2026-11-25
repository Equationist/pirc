/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Strict ISO C17 (ISO/IEC 9899:2018) conformance test reporting harness
 * (inspired by ACVC REPORT).
 */
#ifndef C17_TEST_H
#define C17_TEST_H

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *c17_test_name_ = "unknown";
static int c17_checks_total_ = 0;
static int c17_checks_failed_ = 0;

static void c17_begin_(const char *name, const char *desc) {
    c17_test_name_ = name;
    c17_checks_total_ = 0;
    c17_checks_failed_ = 0;
    printf("---- %s: %s ----\n", name, desc);
}

static void c17_fail_(int line, const char *expr, const char *msg) {
    c17_checks_failed_++;
    if (msg && msg[0] != '\0') {
        printf("   * FAIL line %d: %s (%s)\n", line, expr, msg);
    } else {
        printf("   * FAIL line %d: %s\n", line, expr);
    }
}

static void c17_check_(int cond, int line, const char *expr) {
    c17_checks_total_++;
    if (!cond) {
        c17_fail_(line, expr, "");
    }
}

static void c17_check_ll_(long long got, long long want, int line, const char *expr) {
    c17_checks_total_++;
    if (got != want) {
        c17_checks_failed_++;
        printf("   * FAIL line %d: %s (got %lld, want %lld)\n", line, expr, got, want);
    }
}

static void c17_check_ull_(unsigned long long got, unsigned long long want, int line, const char *expr) {
    c17_checks_total_++;
    if (got != want) {
        c17_checks_failed_++;
        printf("   * FAIL line %d: %s (got 0x%llx, want 0x%llx)\n", line, expr, got, want);
    }
}

static void c17_check_str_(const char *got, const char *want, int line, const char *expr) {
    c17_checks_total_++;
    if (!got || !want || strcmp(got, want) != 0) {
        c17_checks_failed_++;
        printf("   * FAIL line %d: %s (got \"%s\", want \"%s\")\n",
               line, expr, got ? got : "(null)", want ? want : "(null)");
    }
}

static void c17_check_dbl_(double got, double want, double eps, int line, const char *expr) {
    double diff = got - want;
    if (diff < 0.0) {
        diff = -diff;
    }
    c17_checks_total_++;
    if (!(diff <= eps)) {
        c17_checks_failed_++;
        printf("   * FAIL line %d: %s (got %.17g, want %.17g, eps %.3g)\n",
               line, expr, got, want, eps);
    }
}

static int c17_not_applicable_(const char *reason) {
    printf("++++ %s NOT-APPLICABLE (%s) ++++\n", c17_test_name_, reason);
    return 0;
}

static int c17_end_(void) {
    if (c17_checks_failed_ == 0 && c17_checks_total_ > 0) {
        printf("==== %s PASSED (%d checks) ====\n", c17_test_name_, c17_checks_total_);
        return 0;
    }
    if (c17_checks_total_ == 0) {
        printf("**** %s FAILED (no checks executed) ****\n", c17_test_name_);
        return 1;
    }
    printf("**** %s FAILED (%d/%d checks failed) ****\n",
           c17_test_name_, c17_checks_failed_, c17_checks_total_);
    return 1;
}

#define C17_BEGIN(name, desc) c17_begin_((name), (desc))
#define C17_CHECK(cond) c17_check_((cond) ? 1 : 0, __LINE__, #cond)
#define C17_CHECK_EQ(got, want) c17_check_ll_((long long)(got), (long long)(want), __LINE__, #got " == " #want)
#define C17_CHECK_UEQ(got, want) c17_check_ull_((unsigned long long)(got), (unsigned long long)(want), __LINE__, #got " == " #want)
#define C17_CHECK_STR(got, want) c17_check_str_((got), (want), __LINE__, #got " == " #want)
#define C17_CHECK_DBL(got, want, eps) c17_check_dbl_((double)(got), (double)(want), (double)(eps), __LINE__, #got " ~= " #want)
#define C17_FAIL(msg) do { c17_checks_total_++; c17_fail_(__LINE__, "FAIL", (msg)); } while (0)
#define C17_NOT_APPLICABLE(reason) return c17_not_applicable_((reason))
#define C17_END() return c17_end_()

/* Backwards-compatible aliases for tests inherited from C99 clauses */
#define C99_BEGIN C17_BEGIN
#define C99_CHECK C17_CHECK
#define C99_CHECK_EQ C17_CHECK_EQ
#define C99_CHECK_UEQ C17_CHECK_UEQ
#define C99_CHECK_STR C17_CHECK_STR
#define C99_CHECK_DBL C17_CHECK_DBL
#define C99_FAIL C17_FAIL
#define C99_END C17_END

/* Prevent constant folding by hiding values through volatile */
static int c17_opaque_int(int x) {
    volatile int v = x;
    return v;
}

static long long c17_opaque_ll(long long x) {
    volatile long long v = x;
    return v;
}

static unsigned long long c17_opaque_ull(unsigned long long x) {
    volatile unsigned long long v = x;
    return v;
}

static double c17_opaque_dbl(double x) {
    volatile double v = x;
    return v;
}

static float c17_opaque_flt(float x) {
    volatile float v = x;
    return v;
}

#define c99_opaque_int c17_opaque_int
#define c99_opaque_ll  c17_opaque_ll
#define c99_opaque_ull c17_opaque_ull
#define c99_opaque_dbl c17_opaque_dbl
#define c99_opaque_flt c17_opaque_flt

static void c17_use_helpers_(void) {
    (void)c17_opaque_int;
    (void)c17_opaque_ll;
    (void)c17_opaque_ull;
    (void)c17_opaque_dbl;
    (void)c17_opaque_flt;
    (void)c17_check_;
    (void)c17_check_ll_;
    (void)c17_check_ull_;
    (void)c17_check_str_;
    (void)c17_check_dbl_;
    (void)c17_fail_;
    (void)c17_not_applicable_;
    (void)c17_use_helpers_;
}

#endif /* C17_TEST_H */
