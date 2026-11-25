/*
 * C17 §7.5 (<errno.h>), §7.14 (<signal.h>), §7.21 (<stdio.h> hex float & length
 * modifiers), §7.22 (<stdlib.h> bsearch/qsort), §7.24 (<string.h> memmove/strchr).
 */
#include "c17_test.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static volatile sig_atomic_t g_sig_hits = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_sig_hits = g_sig_hits + 1;
}

struct Rec {
    int key;
    int val;
};

static int cmp_rec(const void *a, const void *b) {
    const struct Rec *ra = (const struct Rec *)a;
    const struct Rec *rb = (const struct Rec *)b;
    return (ra->key > rb->key) - (ra->key < rb->key);
}

int main(void) {
    C17_BEGIN("c7_stdlib_edge_gotchas", "memmove overlap, strchr NUL, printf %a/%zu/%hhd, bsearch, errno, signal");

    /* Overlapping memmove (both forward and backward) */
    char m[16] = "abcdefghij";
    memmove(m + 2, m, 8); /* forward overlap */
    m[10] = '\0';
    C17_CHECK_STR(m, "ababcdefgh");
    memmove(m, m + 2, 8); /* backward overlap */
    m[8] = '\0';
    C17_CHECK_STR(m, "abcdefgh");

    /* strchr MUST find terminating NUL (§7.24.5.2) */
    const char *s = "hello";
    const char *nul = strchr(s, '\0');
    C17_CHECK(nul == s + 5);

    /* C99/C17 printf length modifiers (%hhd, %hhu, %zu, %td, %jd) and %a hex float */
    char buf[128];
    size_t sz = 12345;
    ptrdiff_t pd = -77;
    intmax_t jm = -999999999999LL;
    snprintf(buf, sizeof(buf), "%hhd:%hhu:%zu:%td:%jd:%.*s",
             (signed char)-5, (unsigned char)250, sz, pd, jm, 3, "abcdef");
    C17_CHECK_STR(buf, "-5:250:12345:-77:-999999999999:abc");

    /* sscanf hex floating point */
    double parsed = 0.0;
    C17_CHECK_EQ(sscanf("0x1.fp+3", "%lf", &parsed), 1);
    C17_CHECK_DBL(parsed, 15.5, 0.0);

    /* qsort + bsearch */
    struct Rec recs[5] = { {30, 3}, {10, 1}, {50, 5}, {20, 2}, {40, 4} };
    qsort(recs, 5, sizeof(recs[0]), cmp_rec);
    struct Rec key = { 40, 0 };
    struct Rec *found = (struct Rec *)bsearch(&key, recs, 5, sizeof(recs[0]), cmp_rec);
    C17_CHECK(found != NULL);
    C17_CHECK_EQ(found->val, 4);

    /* errno ERANGE on strtoll overflow */
    errno = 0;
    long long ov = strtoll("999999999999999999999999999999", NULL, 10);
    C17_CHECK_EQ(ov, LLONG_MAX);
    C17_CHECK_EQ(errno, ERANGE);

    /* <signal.h> raise() + volatile sig_atomic_t */
    C17_CHECK(signal(SIGUSR1, sig_handler) != SIG_ERR);
    C17_CHECK_EQ(raise(SIGUSR1), 0);
    C17_CHECK_EQ(g_sig_hits, 1);

    C17_END();
}
