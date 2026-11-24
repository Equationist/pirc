/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "common.h"
#include <pthread.h>

#ifdef __APPLE__
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#endif

/* 1. Weak linkage checks */
extern int32_t weak_overridden_by_c(void);
extern int32_t weak_only_in_pir(void);
extern int32_t weak_both_sides(void);

/* Strong C definition MUST override PIR's `weak func @weak_overridden_by_c` */
int32_t weak_overridden_by_c(void) {
    return 77;
}

/* Weak C definition MUST deduplicate cleanly with PIR's `weak func @weak_both_sides` */
__attribute__((weak)) int32_t weak_both_sides(void) {
    return 55;
}

/* 2. Custom Mach-O section descriptor from C side alongside 2 descriptors from PIR */
#ifdef __APPLE__
__attribute__((used, aligned(8), section("__DATA,__pir_reg")))
static const uint64_t c_reg_desc = 0x00000000CAFE0003ULL;
#endif

/* 3. Darwin TLS isolation checks (PIR owns `tls i64`) */
extern int64_t pir_tls_get(void);
extern void    pir_tls_set(int64_t v);

typedef struct {
    int64_t initial_seen;
    int64_t after_set;
} TlsReport;

static void *tls_worker(void *arg) {
    TlsReport *rep = (TlsReport *)arg;
    rep->initial_seen = pir_tls_get();
    pir_tls_set(9999);
    rep->after_set = pir_tls_get();
    return NULL;
}

int main(void) {
    /* Weak tests */
    CHECK_EQ_I64(weak_overridden_by_c(), 77);
    CHECK_EQ_I64(weak_only_in_pir(), 33);
    CHECK_EQ_I64(weak_both_sides(), 55);

    /* Custom Mach-O section test */
#ifdef __APPLE__
    unsigned long sec_size = 0;
    uint8_t *sec_ptr = getsectiondata(&_mh_execute_header, "__DATA", "__pir_reg", &sec_size);
    CHECK(sec_ptr != NULL);
    CHECK_EQ_U64(sec_size, 24ULL); /* 3 x 8-byte uint64_t descriptors */
    int seen1 = 0, seen2 = 0, seen3 = 0;
    const uint64_t *entries = (const uint64_t *)sec_ptr;
    for (unsigned long i = 0; i < sec_size / sizeof(uint64_t); ++i) {
        if (entries[i] == 0x00000000CAFE0001ULL) seen1 = 1;
        if (entries[i] == 0x00000000CAFE0002ULL) seen2 = 1;
        if (entries[i] == 0x00000000CAFE0003ULL) seen3 = 1;
    }
    CHECK(seen1 && seen2 && seen3);
#endif

    /* TLS isolation across pthreads */
    CHECK_EQ_I64(pir_tls_get(), 123);
    pir_tls_set(456);
    CHECK_EQ_I64(pir_tls_get(), 456);

    TlsReport rep = { 0, 0 };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, tls_worker, &rep) == 0);
    CHECK(pthread_join(th, NULL) == 0);

    /* Worker thread must see fresh initial value 123 and not clobber main thread's 456 */
    CHECK_EQ_I64(rep.initial_seen, 123);
    CHECK_EQ_I64(rep.after_set, 9999);
    CHECK_EQ_I64(pir_tls_get(), 456);

    printf("PASS\n");
    return 0;
}
