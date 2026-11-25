/* C17-LDFLAGS: -lpthread */
/*
 * C17 §7.17: Multi-threaded atomics stress test — 4 POSIX threads performing
 * concurrent atomic_fetch_add, CAS-loop updates, atomic_flag spinlock mutual
 * exclusion, and release/acquire publication.
 */
#include "c17_test.h"
#include <pthread.h>
#include <stdatomic.h>

#define NTHREADS 4
#define ITERS 25000

static _Atomic long long g_counter = 0;
static _Atomic int g_cas_counter = 0;
static atomic_flag g_spinlock = ATOMIC_FLAG_INIT;
static long long g_protected_sum = 0;

static void *atomic_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        atomic_fetch_add_explicit(&g_counter, 1, memory_order_relaxed);

        /* CAS-loop increment */
        int cur = atomic_load_explicit(&g_cas_counter, memory_order_relaxed);
        while (!atomic_compare_exchange_weak_explicit(
                   &g_cas_counter, &cur, cur + 1,
                   memory_order_acq_rel, memory_order_relaxed)) {
        }

        /* atomic_flag spinlock protecting non-atomic shared state */
        while (atomic_flag_test_and_set_explicit(&g_spinlock, memory_order_acquire)) {
        }
        g_protected_sum += 3;
        atomic_flag_clear_explicit(&g_spinlock, memory_order_release);
    }
    atomic_thread_fence(memory_order_seq_cst);
    return NULL;
}

int main(void) {
    C17_BEGIN("c717_atomics_pthreads", "C17 multi-threaded atomics, CAS loop, and atomic_flag spinlock");

    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        C17_CHECK_EQ(pthread_create(&th[i], NULL, atomic_worker, NULL), 0);
    }
    for (int i = 0; i < NTHREADS; i++) {
        C17_CHECK_EQ(pthread_join(th[i], NULL), 0);
    }

    C17_CHECK_EQ(atomic_load(&g_counter), (long long)NTHREADS * ITERS);
    C17_CHECK_EQ(atomic_load(&g_cas_counter), NTHREADS * ITERS);
    C17_CHECK_EQ(g_protected_sum, 3LL * NTHREADS * ITERS);

    C17_END();
}
