/* C17-LDFLAGS: -lpthread */
/*
 * C17 §6.2.4 & §6.7.1: _Thread_local storage duration — zero-initialized,
 * nonzero-initialized, and function-scope static _Thread_local variables
 * verified across 4 concurrent POSIX threads.
 */
#include "c17_test.h"
#include <pthread.h>

static _Thread_local int tls_zero;
static _Thread_local long long tls_init = 1000LL;

static int bump_local_tls(int delta) {
    static _Thread_local int fn_tls = 50;
    fn_tls += delta;
    return fn_tls;
}

struct ThreadArg {
    int tid;
    long long out_sum;
    int ok;
};

static void *worker(void *arg) {
    struct ThreadArg *ta = (struct ThreadArg *)arg;
    int id = ta->tid;

    /* Initial values must be fresh per thread */
    int ok = (tls_zero == 0) && (tls_init == 1000LL);

    for (int i = 1; i <= 100; i++) {
        tls_zero += id;
        tls_init += i;
    }
    int loc1 = bump_local_tls(id * 10);
    int loc2 = bump_local_tls(id);

    /* Expected: tls_zero = 100*id, tls_init = 1000 + 5050 = 6050,
     * loc1 = 50 + 10*id, loc2 = 50 + 11*id */
    if (tls_zero != 100 * id || tls_init != 6050LL ||
        loc1 != 50 + 10 * id || loc2 != 50 + 11 * id) {
        ok = 0;
    }
    ta->out_sum = (long long)tls_zero + tls_init + loc2;
    ta->ok = ok;
    return NULL;
}

int main(void) {
    C17_BEGIN("c62_thread_local_pthreads", "C17 _Thread_local multi-threaded isolation");

    /* Mutate main thread's TLS before spawning workers */
    tls_zero = 777;
    tls_init = 9999LL;
    C17_CHECK_EQ(bump_local_tls(5), 55);

    pthread_t th[4];
    struct ThreadArg args[4];
    for (int i = 0; i < 4; i++) {
        args[i].tid = i + 1;
        args[i].out_sum = 0;
        args[i].ok = 0;
        C17_CHECK_EQ(pthread_create(&th[i], NULL, worker, &args[i]), 0);
    }
    for (int i = 0; i < 4; i++) {
        C17_CHECK_EQ(pthread_join(th[i], NULL), 0);
        C17_CHECK_EQ(args[i].ok, 1);
    }

    /* Main thread's TLS must be completely undisturbed */
    C17_CHECK_EQ(tls_zero, 777);
    C17_CHECK_EQ(tls_init, 9999LL);
    C17_CHECK_EQ(bump_local_tls(1), 56);

    C17_END();
}
