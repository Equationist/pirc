/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Ada 83 Chapter 9 Tasking Runtime (POSIX pthreads).
 *
 * Implements:
 *   - Task creation, activation protocol (LRM 9.3), and TASKING_ERROR on activation failure
 *   - Master-dependent waiting and distributed termination (LRM 9.4)
 *   - Entries, entry families, FIFO queues, rendezvous parameter passing, and
 *     dual exception propagation to caller + acceptor (LRM 9.5, 11.5)
 *   - Delay statements (LRM 9.6)
 *   - Selective wait with guards, accept, delay, else, terminate, and PROGRAM_ERROR
 *     when all alternatives are closed (LRM 9.7.1)
 *   - Conditional and timed entry calls (LRM 9.7.2, 9.7.3)
 *   - Abort statements (LRM 9.10)
 *   - Attributes 'CALLABLE, 'TERMINATED, 'COUNT (LRM 9.9)
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern void __ada_raise(const void *exc_tag);
extern void *__ada_install_stack_overflow_handler(void);
extern void __ada_uninstall_stack_overflow_handler(void *alt_buf);
extern const char __ada_exc_tasking_error[];
extern const char __ada_exc_program_error[];
extern const char __ada_exc_storage_error[];

/* Entry call states */
enum {
    EC_WAITING       = 0,
    EC_ACCEPTED      = 1,
    EC_DONE          = 2,
    EC_TASKING_ERROR = 3,
    EC_CANCELLED     = 4
};

/* Selective wait alternative kinds */
enum {
    ALT_CLOSED    = 0,
    ALT_ACCEPT    = 1,
    ALT_DELAY     = 2,
    ALT_TERMINATE = 3
};

/* Task lifecycle states */
enum {
    TS_CREATED           = 0,
    TS_ACTIVATING        = 1,
    TS_RUNNING           = 2,
    TS_COMPLETED         = 3,
    TS_TERMINATED        = 4,
    TS_ABNORMAL          = 5,
    TS_ACTIVATION_FAILED = 6
};

typedef struct AdaActGroup {
    int32_t pending;
    int32_t errors;
} AdaActGroup;

typedef struct AdaEntryCall {
    int32_t               entry_idx;
    void                 *params;
    int                   state;
    int                   is_conditional;
    int                   refcnt;
    int32_t               caller_prio;
    int                   saved_acc_has_prio;
    int32_t               saved_acc_prio;
    const void           *raised_exc;
    struct AdaEntryCall  *next;
    struct AdaEntryCall  *prev_active;
} AdaEntryCall;

static int32_t g_main_priority = -1;
static int32_t current_caller_priority(void);

static AdaEntryCall *entry_call_alloc(int32_t entry_idx, void *params, int is_cond) {
    AdaEntryCall *c = (AdaEntryCall *)calloc(1, sizeof(AdaEntryCall));
    if (!c) __ada_raise(__ada_exc_storage_error);
    c->entry_idx = entry_idx;
    c->params = params;
    c->state = EC_WAITING;
    c->is_conditional = is_cond;
    c->refcnt = 1;
    c->caller_prio = current_caller_priority();
    return c;
}

static void entry_call_retain(AdaEntryCall *c) {
    if (c) __atomic_add_fetch(&c->refcnt, 1, __ATOMIC_RELAXED);
}

static void entry_call_release(AdaEntryCall *c) {
    if (c && __atomic_sub_fetch(&c->refcnt, 1, __ATOMIC_ACQ_REL) == 0) {
        free(c);
    }
}

typedef struct AdaEntryQueue {
    AdaEntryCall *head;
    AdaEntryCall *tail;
    int32_t       count;
} AdaEntryQueue;

struct AdaMaster;

typedef struct AdaTask {
    pthread_t          thread;
    int                thread_started;
    pthread_mutex_t    mtx;
    pthread_cond_t     cv;

    int                state;
    int                activating;
    int                is_callable;
    int                is_terminated;
    int                aborted;
    int                waiting_terminate;
    int                terminate_committed;
    int                has_priority;
    int32_t            priority;
    size_t             stack_size;
    uint8_t           *body_elab_flag;
    int64_t            group_id;
    AdaActGroup       *act_group;

    void             (*body_fn)(void *env, void *self_task);
    void              *env;

    int32_t            num_entries;
    AdaEntryQueue     *entries;
    uint8_t           *open_accept; /* 1 if currently open in accept / selective wait */
    AdaEntryCall      *active_calls; /* stack of calls currently inside ACCEPT body */

    struct AdaMaster  *master;
    struct AdaTask    *next_in_master;
    struct AdaMaster  *child_masters; /* masters owned by this task (for recursive abort) */

    /* Tracking while this task is blocked waiting in another task's entry queue */
    struct AdaTask    *waiting_callee;
    int32_t            waiting_entry;
    AdaEntryCall      *waiting_call;
} AdaTask;

typedef struct AdaMaster {
    pthread_mutex_t   mtx;
    pthread_cond_t    cv;
    AdaTask          *tasks;
    int32_t           total_tasks;
    int32_t           activated_done;
    int32_t           activation_errors;
    int32_t           active_count;
    int32_t           waiting_terminate;
    int               master_completed;
    int               terminate_now;
    struct AdaMaster *parent;
    struct AdaTask   *owner_task;
    struct AdaMaster *next_child;
} AdaMaster;

static __thread AdaTask *g_current_task = NULL;
static __thread void *g_task_alt_stk = NULL;
static __thread void *g_active_master = NULL;

typedef struct AdaAllocFrame {
    AdaMaster *master;
    int64_t    gid;
} AdaAllocFrame;

static int64_t g_next_alloc_gid = 1000000000LL;
static __thread AdaAllocFrame g_alloc_stack[32];
static __thread int g_alloc_sp = 0;

/* Global master for library-level tasks */
static AdaMaster g_global_master;
static pthread_once_t g_global_once = PTHREAD_ONCE_INIT;

static void apply_thread_priority(pthread_t thr, int32_t prio) {
    int policy = SCHED_OTHER;
    struct sched_param sp;
    if (pthread_getschedparam(thr, &policy, &sp) == 0) {
        int pmin = sched_get_priority_min(policy);
        int pmax = sched_get_priority_max(policy);
        if (pmin >= 0 && pmax >= pmin) {
            int p = prio;
            if (p < 0) p = 0;
            if (p > 10) p = 10;
            sp.sched_priority = pmin + ((pmax - pmin) * p) / 10;
            (void)pthread_setschedparam(thr, policy, &sp);
        }
    }
}

void __ada_set_main_priority(int32_t prio) {
    __atomic_store_n(&g_main_priority, prio, __ATOMIC_RELEASE);
    apply_thread_priority(pthread_self(), prio);
}

static int32_t current_caller_priority(void) {
    AdaTask *self = g_current_task;
    if (self) {
        pthread_mutex_lock(&self->mtx);
        int32_t p = self->has_priority ? self->priority : -1;
        pthread_mutex_unlock(&self->mtx);
        return p;
    }
    return __atomic_load_n(&g_main_priority, __ATOMIC_ACQUIRE);
}

void *__ada_global_master(void);

void *__ada_get_active_master(void) {
    if (g_active_master != NULL) return g_active_master;
    return __ada_global_master();
}

void *__ada_set_active_master(void *m) {
    void *prev = g_active_master;
    g_active_master = m;
    return prev;
}

static void init_master_fields(AdaMaster *m, AdaMaster *parent) {
    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->mtx, NULL);
    pthread_cond_init(&m->cv, NULL);
    m->parent = parent;
}

static void init_global_master(void) {
    init_master_fields(&g_global_master, NULL);
}

void *__ada_global_master(void) {
    pthread_once(&g_global_once, init_global_master);
    return &g_global_master;
}

void *__ada_task_self(void) {
    return g_current_task;
}

static int64_t clamp_delay_ns(int64_t ns) {
    /* Cap to ~100 years so get_monotonic_ns() + ns and timespec tv_sec never overflow */
    const int64_t MAX_DELAY_NS = 3153600000LL * 1000000000LL;
    if (ns <= 0) return 0;
    if (ns > MAX_DELAY_NS) return MAX_DELAY_NS;
    return ns;
}

static void add_ns_to_timespec(struct timespec *ts, int64_t ns) {
    ns = clamp_delay_ns(ns);
    if (ns <= 0) return;
    int64_t sec = ns / 1000000000LL;
    int64_t rem = ns % 1000000000LL;
    ts->tv_sec += (time_t)sec;
    ts->tv_nsec += (long)rem;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int64_t get_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void get_realtime_deadline(struct timespec *ts, int64_t delay_ns) {
    clock_gettime(CLOCK_REALTIME, ts);
    add_ns_to_timespec(ts, delay_ns);
}

static void task_account_activation_locked(AdaMaster *m, AdaTask *t, int failed) {
    /* Caller holds m->mtx */
    m->activated_done++;
    if (failed) {
        m->activation_errors++;
    }
    if (t->act_group != NULL) {
        t->act_group->pending--;
        if (failed) {
            t->act_group->errors++;
        }
        t->act_group = NULL;
    }
    pthread_cond_broadcast(&m->cv);
}

static void *task_thread_trampoline(void *arg);

static int spawn_task_thread(AdaTask *t) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (t->stack_size > 0) {
        size_t req = t->stack_size;
        size_t min_sz = (size_t)PTHREAD_STACK_MIN;
        if (req < min_sz) req = min_sz;
        long pgsz = sysconf(_SC_PAGESIZE);
        if (pgsz > 0) {
            size_t p = (size_t)pgsz;
            req = (req + p - 1u) & ~(p - 1u);
        }
        (void)pthread_attr_setstacksize(&attr, req);
    }
    if (t->has_priority) {
        int policy = SCHED_OTHER;
        struct sched_param sp;
        if (pthread_attr_getschedpolicy(&attr, &policy) == 0) {
            int pmin = sched_get_priority_min(policy);
            int pmax = sched_get_priority_max(policy);
            if (pmin >= 0 && pmax >= pmin) {
                int p = t->priority;
                if (p < 0) p = 0;
                if (p > 10) p = 10;
                sp.sched_priority = pmin + ((pmax - pmin) * p) / 10;
                (void)pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
                (void)pthread_attr_setschedparam(&attr, &sp);
            }
        }
    }
    int rc = pthread_create(&t->thread, &attr, task_thread_trampoline, t);
    if (rc != 0 && t->has_priority) {
        (void)pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
        rc = pthread_create(&t->thread, &attr, task_thread_trampoline, t);
    }
    pthread_attr_destroy(&attr);
    return rc;
}

static void cancel_queued_calls_only(AdaTask *t) {
    /* Caller holds t->mtx: cancels queued (not yet accepted) calls only */
    for (int32_t i = 0; i < t->num_entries; ++i) {
        AdaEntryCall *c = t->entries[i].head;
        while (c) {
            AdaEntryCall *nxt = c->next;
            c->next = NULL;
            c->state = EC_TASKING_ERROR;
            entry_call_release(c);
            c = nxt;
        }
        t->entries[i].head = NULL;
        t->entries[i].tail = NULL;
        t->entries[i].count = 0;
    }
    pthread_cond_broadcast(&t->cv);
}

static void cancel_active_calls_only(AdaTask *t) {
    /* Caller holds t->mtx: only safe when acceptor has permanently exited its accept bodies! */
    AdaEntryCall *ac = t->active_calls;
    while (ac) {
        AdaEntryCall *prev = ac->prev_active;
        ac->prev_active = NULL;
        if (ac->state == EC_ACCEPTED) {
            ac->state = EC_TASKING_ERROR;
        }
        entry_call_release(ac);
        ac = prev;
    }
    t->active_calls = NULL;
    pthread_cond_broadcast(&t->cv);
}

static void cancel_all_queued_calls(AdaTask *t) {
    /* Caller holds t->mtx */
    cancel_queued_calls_only(t);
    cancel_active_calls_only(t);
}

static pthread_mutex_t g_quiesce_mtx = PTHREAD_MUTEX_INITIALIZER;

static int task_has_pending_open_accept_locked(AdaTask *t) {
    for (int32_t e = 0; e < t->num_entries; ++e) {
        if (t->open_accept[e] && t->entries[e].count > 0) {
            return 1;
        }
    }
    return 0;
}

/* Lock-all-then-commit quiescence checker (LRM 9.4(6), P3-4.4):
 * Holds g_quiesce_mtx -> m->mtx -> t->mtx -> child_m->mtx in deterministic list order
 * across BOTH verification and atomic commit (t->terminate_committed = 1, t->is_callable = 0),
 * eliminating TOCTOU races and irreversible m->terminate_now premature sibling kills! */
typedef struct QuiesceLocks {
    AdaMaster **masters;
    int         n_masters;
    int         cap_masters;
    AdaTask   **tasks;
    int         n_tasks;
    int         cap_tasks;
} QuiesceLocks;

static void ql_init(QuiesceLocks *ql) {
    memset(ql, 0, sizeof(*ql));
}

static void ql_push_master(QuiesceLocks *ql, AdaMaster *m) {
    if (ql->n_masters >= ql->cap_masters) {
        int ncap = ql->cap_masters ? ql->cap_masters * 2 : 32;
        AdaMaster **nm = (AdaMaster **)realloc(ql->masters, (size_t)ncap * sizeof(AdaMaster *));
        if (!nm) return;
        ql->masters = nm;
        ql->cap_masters = ncap;
    }
    ql->masters[ql->n_masters++] = m;
}

static void ql_push_task(QuiesceLocks *ql, AdaTask *t) {
    if (ql->n_tasks >= ql->cap_tasks) {
        int ncap = ql->cap_tasks ? ql->cap_tasks * 2 : 64;
        AdaTask **nt = (AdaTask **)realloc(ql->tasks, (size_t)ncap * sizeof(AdaTask *));
        if (!nt) return;
        ql->tasks = nt;
        ql->cap_tasks = ncap;
    }
    ql->tasks[ql->n_tasks++] = t;
}

static void ql_unlock_all(QuiesceLocks *ql) {
    for (int i = ql->n_tasks - 1; i >= 0; --i) {
        pthread_mutex_unlock(&ql->tasks[i]->mtx);
    }
    for (int i = ql->n_masters - 1; i >= 0; --i) {
        pthread_mutex_unlock(&ql->masters[i]->mtx);
    }
    free(ql->tasks);
    free(ql->masters);
}

static int master_tree_lock_and_check(AdaMaster *m, int require_completed, QuiesceLocks *ql) {
    /* Caller holds g_quiesce_mtx; locks m and all subtree tasks/child_masters in deterministic order */
    pthread_mutex_lock(&m->mtx);
    ql_push_master(ql, m);
    if (require_completed && !m->master_completed) return 0;
    for (AdaTask *t = m->tasks; t != NULL; t = t->next_in_master) {
        pthread_mutex_lock(&t->mtx);
        ql_push_task(ql, t);
        if (!t->is_terminated) {
            if ((!t->waiting_terminate && t->state != TS_COMPLETED)
                || task_has_pending_open_accept_locked(t)) {
                return 0;
            }
            for (AdaMaster *cm = t->child_masters; cm != NULL; cm = cm->next_child) {
                if (!master_tree_lock_and_check(cm, 0, ql)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static void check_quiescence_tree_under_qmtx(AdaMaster *start) {
    /* Caller holds g_quiesce_mtx (and NO m->mtx or t->mtx) */
    if (!start) return;
    AdaMaster *chain[64];
    int depth = 0;
    AdaMaster *cur = start;
    while (cur && depth < 64) {
        chain[depth++] = cur;
        AdaTask *own = cur->owner_task;
        cur = (own != NULL) ? own->master : NULL;
    }
    for (int i = depth - 1; i >= 0; --i) {
        AdaMaster *m = chain[i];
        QuiesceLocks ql;
        ql_init(&ql);
        int ok = master_tree_lock_and_check(m, 1, &ql);
        if (ok && m->active_count > 0) {
            /* Atomically commit termination while holding ALL task & master locks in subtree! */
            for (int ti = 0; ti < ql.n_tasks; ++ti) {
                AdaTask *t = ql.tasks[ti];
                if (!t->is_terminated && t->waiting_terminate) {
                    t->terminate_committed = 1;
                    t->is_callable = 0;
                    cancel_queued_calls_only(t);
                    pthread_cond_broadcast(&t->cv);
                }
            }
            ql_unlock_all(&ql);
            break;
        }
        ql_unlock_all(&ql);
    }
}

static void check_quiescence_tree(AdaMaster *start) {
    if (!start) return;
    pthread_mutex_lock(&g_quiesce_mtx);
    check_quiescence_tree_under_qmtx(start);
    pthread_mutex_unlock(&g_quiesce_mtx);
}

static void task_finish_cleanup(AdaTask *self) {
    /* Mark task terminated and notify any waiting callers & master */
    pthread_mutex_lock(&self->mtx);
    int was_activating = self->activating || (self->state == TS_ACTIVATING || self->state == TS_CREATED);
    self->activating = 0;
    int was_aborted = __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE);
    self->is_callable = 0;
    self->is_terminated = 1;
    self->state = TS_TERMINATED;
    cancel_all_queued_calls(self);
    pthread_mutex_unlock(&self->mtx);

    AdaMaster *m = self->master;
    if (m) {
        pthread_mutex_lock(&g_quiesce_mtx);
        pthread_mutex_lock(&m->mtx);
        if (was_activating) {
            task_account_activation_locked(m, self, was_aborted);
        }
        if (m->active_count > 0) {
            m->active_count--;
        }
        int still_active = (m->active_count > 0);
        pthread_cond_broadcast(&m->cv);
        pthread_mutex_unlock(&m->mtx);
        if (still_active || m->owner_task != NULL) {
            check_quiescence_tree_under_qmtx(m);
        }
        pthread_mutex_unlock(&g_quiesce_mtx);
    }
    g_current_task = NULL;
}

void __ada_task_completed(void *self_ptr);
void __ada_master_abort_unactivated(void *master_ptr);
void __ada_master_abort_since(void *master_ptr, void *mark);

static void abort_exit_self(AdaTask *self) {
    /* LRM 9.10(6) & 9.4(5) (P3-4.2, P3-4.13): abort any in-flight allocator group tasks
       created by this thread, become completed until child masters terminate, clean up altstack! */
    while (g_alloc_sp > 0) {
        AdaAllocFrame af = g_alloc_stack[g_alloc_sp - 1];
        __ada_master_abort_since(af.master, (void *)(uintptr_t)af.gid);
    }
    pthread_mutex_lock(&self->mtx);
    cancel_active_calls_only(self);
    pthread_mutex_unlock(&self->mtx);
    __ada_task_completed(self);
    for (;;) {
        pthread_mutex_lock(&self->mtx);
        AdaMaster *cm = self->child_masters;
        pthread_mutex_unlock(&self->mtx);
        if (!cm) break;
        __ada_master_abort_unactivated(cm);
    }
    task_finish_cleanup(self);
    if (g_task_alt_stk != NULL) {
        void *stk = g_task_alt_stk;
        g_task_alt_stk = NULL;
        __ada_uninstall_stack_overflow_handler(stk);
    }
    pthread_exit(NULL);
}

void __ada_check_abort(void) {
    AdaTask *self = g_current_task;
    if (self && __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) {
        abort_exit_self(self);
    }
}

static void *task_thread_trampoline(void *arg) {
    AdaTask *self = (AdaTask *)arg;
    g_current_task = self;
    void *alt_stk = __ada_install_stack_overflow_handler();
    g_task_alt_stk = alt_stk;

    /* Run Ada task body (it calls __ada_task_activated once its declarative part finishes) */
    if (self->body_fn) {
        self->body_fn(self->env, self);
    }

    task_finish_cleanup(self);
    g_task_alt_stk = NULL;
    __ada_uninstall_stack_overflow_handler(alt_stk);
    return NULL;
}

/* =========================================================================
 * Master Management
 * ========================================================================= */

static void unlink_child_master(AdaMaster *m) {
    AdaTask *owner = m->owner_task;
    if (!owner) return;
    pthread_mutex_lock(&owner->mtx);
    AdaMaster **pp = &owner->child_masters;
    while (*pp) {
        if (*pp == m) {
            *pp = m->next_child;
            break;
        }
        pp = &(*pp)->next_child;
    }
    m->owner_task = NULL;
    m->next_child = NULL;
    pthread_mutex_unlock(&owner->mtx);
}

void *__ada_master_enter(void) {
    AdaMaster *m = (AdaMaster *)calloc(1, sizeof(AdaMaster));
    if (!m) __ada_raise(__ada_exc_storage_error);
    init_master_fields(m, NULL);
    AdaTask *self = g_current_task;
    if (self) {
        pthread_mutex_lock(&self->mtx);
        m->owner_task = self;
        m->next_child = self->child_masters;
        self->child_masters = m;
        pthread_mutex_unlock(&self->mtx);
    }
    return m;
}

void __ada_task_set_elab_flag(void *task_ptr, uint8_t *flag) {
    AdaTask *t = (AdaTask *)task_ptr;
    if (!t) return;
    pthread_mutex_lock(&t->mtx);
    t->body_elab_flag = flag;
    pthread_mutex_unlock(&t->mtx);
}

void __ada_task_set_group(void *task_ptr, int64_t gid) {
    AdaTask *t = (AdaTask *)task_ptr;
    if (!t) return;
    /* P3-4.2: Do not overwrite an active allocator group_id on this task's master! */
    if (g_alloc_sp > 0 && g_alloc_stack[g_alloc_sp - 1].master == t->master) {
        return;
    }
    pthread_mutex_lock(&t->mtx);
    t->group_id = gid;
    pthread_mutex_unlock(&t->mtx);
}

void __ada_task_set_priority(void *task_ptr, int32_t prio) {
    AdaTask *t = (AdaTask *)task_ptr;
    if (!t) return;
    pthread_mutex_lock(&t->mtx);
    t->has_priority = 1;
    t->priority = prio;
    pthread_mutex_unlock(&t->mtx);
}

void __ada_task_set_stack_size(void *task_ptr, int64_t bytes) {
    AdaTask *t = (AdaTask *)task_ptr;
    if (!t || bytes <= 0) return;
    pthread_mutex_lock(&t->mtx);
    t->stack_size = (size_t)bytes;
    pthread_mutex_unlock(&t->mtx);
}

void __ada_master_activate_group(void *master_ptr, int64_t gid) {
    __ada_check_abort();
    AdaMaster *m = (AdaMaster *)master_ptr;
    if (!m) return;

    pthread_mutex_lock(&m->mtx);
    int unelab = 0;
    for (AdaTask *t = m->tasks; t != NULL; t = t->next_in_master) {
        pthread_mutex_lock(&t->mtx);
        if (t->group_id == gid && t->state == TS_CREATED && t->body_elab_flag != NULL && *(t->body_elab_flag) == 0) {
            unelab = 1;
        }
        pthread_mutex_unlock(&t->mtx);
    }
    if (unelab) {
        for (AdaTask *t = m->tasks; t != NULL; t = t->next_in_master) {
            pthread_mutex_lock(&t->mtx);
            if (t->group_id == gid && t->state == TS_CREATED) {
                t->state = TS_TERMINATED;
                t->is_callable = 0;
                t->is_terminated = 1;
                cancel_all_queued_calls(t);
                m->activated_done++;
                if (m->active_count > 0) m->active_count--;
            }
            pthread_mutex_unlock(&t->mtx);
        }
        pthread_cond_broadcast(&m->cv);
        pthread_mutex_unlock(&m->mtx);
        __ada_raise(__ada_exc_program_error);
        return;
    }
    AdaActGroup grp = { 0, 0 };
    for (AdaTask *t = m->tasks; t != NULL; t = t->next_in_master) {
        pthread_mutex_lock(&t->mtx);
        if (t->group_id == gid && t->state == TS_CREATED) {
            if (__atomic_load_n(&t->aborted, __ATOMIC_ACQUIRE)) {
                t->state = TS_TERMINATED;
                t->is_callable = 0;
                t->is_terminated = 1;
                cancel_all_queued_calls(t);
                m->activated_done++;
                grp.errors++;
                if (m->active_count > 0) m->active_count--;
            } else {
                t->state = TS_ACTIVATING;
                t->activating = 1;
                t->act_group = &grp;
                grp.pending++;
                t->is_callable = 1;
                t->thread_started = 1;
                if (spawn_task_thread(t) != 0) {
                    t->thread_started = 0;
                    t->activating = 0;
                    t->act_group = NULL;
                    grp.pending--;
                    grp.errors++;
                    t->state = TS_TERMINATED;
                    t->is_callable = 0;
                    t->is_terminated = 1;
                    cancel_all_queued_calls(t);
                    m->activated_done++;
                    if (m->active_count > 0) m->active_count--;
                }
            }
        }
        pthread_mutex_unlock(&t->mtx);
    }
    while (grp.pending > 0) {
        pthread_cond_wait(&m->cv, &m->mtx);
    }
    int had_err = (grp.errors > 0);
    pthread_mutex_unlock(&m->mtx);
    __ada_check_abort();
    if (had_err) {
        __ada_raise(__ada_exc_tasking_error);
    }
}

void __ada_master_activate(void *master_ptr) {
    __ada_check_abort();
    AdaMaster *m = (AdaMaster *)master_ptr;
    if (!m) return;

    pthread_mutex_lock(&m->mtx);
    /* LRM 3.9: if any task in this activation group has an unelaborated body,
       NO task in the group is activated; all become terminated and PROGRAM_ERROR is raised. */
    int unelab = 0;
    for (AdaTask *t = m->tasks; t != NULL; t = t->next_in_master) {
        pthread_mutex_lock(&t->mtx);
        if (t->state == TS_CREATED && t->body_elab_flag != NULL && *(t->body_elab_flag) == 0) {
            unelab = 1;
        }
        pthread_mutex_unlock(&t->mtx);
    }
    if (unelab) {
        for (AdaTask *t = m->tasks; t != NULL; t = t->next_in_master) {
            pthread_mutex_lock(&t->mtx);
            if (t->state == TS_CREATED) {
                t->state = TS_TERMINATED;
                t->is_callable = 0;
                t->is_terminated = 1;
                cancel_all_queued_calls(t);
                m->activated_done++;
                if (m->active_count > 0) m->active_count--;
            }
            pthread_mutex_unlock(&t->mtx);
        }
        pthread_cond_broadcast(&m->cv);
        pthread_mutex_unlock(&m->mtx);
        __ada_raise(__ada_exc_program_error);
        return;
    }
    AdaActGroup grp = { 0, 0 };
    for (AdaTask *t = m->tasks; t != NULL; t = t->next_in_master) {
        pthread_mutex_lock(&t->mtx);
        if (t->state == TS_CREATED) {
            if (__atomic_load_n(&t->aborted, __ATOMIC_ACQUIRE)) {
                t->state = TS_TERMINATED;
                t->is_callable = 0;
                t->is_terminated = 1;
                cancel_all_queued_calls(t);
                m->activated_done++;
                grp.errors++;
                if (m->active_count > 0) m->active_count--;
            } else {
                t->state = TS_ACTIVATING;
                t->activating = 1;
                t->act_group = &grp;
                grp.pending++;
                t->is_callable = 1;
                t->thread_started = 1;
                if (spawn_task_thread(t) != 0) {
                    t->thread_started = 0;
                    t->activating = 0;
                    t->act_group = NULL;
                    grp.pending--;
                    grp.errors++;
                    t->state = TS_TERMINATED;
                    t->is_callable = 0;
                    t->is_terminated = 1;
                    cancel_all_queued_calls(t);
                    m->activated_done++;
                    if (m->active_count > 0) m->active_count--;
                }
            }
        }
        pthread_mutex_unlock(&t->mtx);
    }
    while (grp.pending > 0) {
        pthread_cond_wait(&m->cv, &m->mtx);
    }
    int had_err = (grp.errors > 0);
    pthread_mutex_unlock(&m->mtx);
    __ada_check_abort();

    if (had_err) {
        __ada_raise(__ada_exc_tasking_error);
    }
}

void __ada_master_leave(void *master_ptr) {
    AdaMaster *m = (AdaMaster *)master_ptr;
    if (!m) return;

    /* Activate any un-activated tasks if block exited early, or wait for all dependents */
    pthread_mutex_lock(&m->mtx);
    m->master_completed = 1;
    pthread_mutex_unlock(&m->mtx);
    check_quiescence_tree(m);
    pthread_mutex_lock(&m->mtx);
    while (m->active_count > 0) {
        pthread_cond_wait(&m->cv, &m->mtx);
    }
    AdaTask *list = m->tasks;
    pthread_mutex_unlock(&m->mtx);

    for (AdaTask *t = list; t != NULL; t = t->next_in_master) {
        if (t->thread_started) {
            pthread_join(t->thread, NULL);
            t->thread_started = 0;
        }
    }

    if (m != &g_global_master) {
        unlink_child_master(m);
        pthread_mutex_destroy(&m->mtx);
        pthread_cond_destroy(&m->cv);
        free(m);
    }
}

void __ada_master_abort_unactivated(void *master_ptr) {
    AdaMaster *m = (AdaMaster *)master_ptr;
    if (!m) return;

    pthread_mutex_lock(&m->mtx);
    for (AdaTask *t = m->tasks; t != NULL; t = t->next_in_master) {
        pthread_mutex_lock(&t->mtx);
        if (t->state == TS_CREATED) {
            t->state = TS_TERMINATED;
            t->is_callable = 0;
            t->is_terminated = 1;
            cancel_all_queued_calls(t);
            if (m->active_count > 0) m->active_count--;
        }
        pthread_mutex_unlock(&t->mtx);
    }
    m->master_completed = 1;
    pthread_mutex_unlock(&m->mtx);
    check_quiescence_tree(m);
    pthread_mutex_lock(&m->mtx);
    while (m->active_count > 0) {
        pthread_cond_wait(&m->cv, &m->mtx);
    }
    AdaTask *list = m->tasks;
    pthread_mutex_unlock(&m->mtx);

    for (AdaTask *t = list; t != NULL; t = t->next_in_master) {
        if (t->thread_started) {
            pthread_join(t->thread, NULL);
            t->thread_started = 0;
        }
    }

    if (m != &g_global_master) {
        /* Wait for any in-flight quiescence checker referencing m before freeing (P3-4.5) */
        pthread_mutex_lock(&g_quiesce_mtx);
        unlink_child_master(m);
        pthread_mutex_unlock(&g_quiesce_mtx);
        pthread_mutex_destroy(&m->mtx);
        pthread_cond_destroy(&m->cv);
        free(m);
    }
}

void *__ada_master_mark(void *master_ptr) {
    /* P3-4.2: Allocate a unique atomic allocator group_id and push onto TLS stack so
       concurrent allocators on the same master never collide or orphan tasks on abort! */
    AdaMaster *m = (AdaMaster *)master_ptr;
    if (!m) m = (AdaMaster *)__ada_global_master();
    int64_t gid = __atomic_add_fetch(&g_next_alloc_gid, 1, __ATOMIC_RELAXED);
    if (g_alloc_sp < 32) {
        g_alloc_stack[g_alloc_sp].master = m;
        g_alloc_stack[g_alloc_sp].gid = gid;
        g_alloc_sp++;
    }
    return (void *)(uintptr_t)gid;
}

static void pop_alloc_frame(int64_t gid) {
    for (int i = g_alloc_sp - 1; i >= 0; --i) {
        if (g_alloc_stack[i].gid == gid) {
            for (int j = i; j < g_alloc_sp - 1; ++j) {
                g_alloc_stack[j] = g_alloc_stack[j + 1];
            }
            g_alloc_sp--;
            break;
        }
    }
}

void __ada_master_activate_since(void *master_ptr, void *mark) {
    int64_t gid = (int64_t)(uintptr_t)mark;
    pop_alloc_frame(gid);
    AdaMaster *m = (AdaMaster *)master_ptr;
    if (!m) m = (AdaMaster *)__ada_global_master();
    __ada_master_activate_group(m, gid);
}

void __ada_master_abort_since(void *master_ptr, void *mark) {
    int64_t gid = (int64_t)(uintptr_t)mark;
    pop_alloc_frame(gid);
    AdaMaster *m = (AdaMaster *)master_ptr;
    if (!m) m = (AdaMaster *)__ada_global_master();
    pthread_mutex_lock(&g_quiesce_mtx);
    pthread_mutex_lock(&m->mtx);
    for (AdaTask *t = m->tasks; t != NULL; t = t->next_in_master) {
        pthread_mutex_lock(&t->mtx);
        if (t->group_id == gid && t->state == TS_CREATED) {
            t->state = TS_TERMINATED;
            t->is_callable = 0;
            t->is_terminated = 1;
            cancel_all_queued_calls(t);
            m->activated_done++;
            if (m->active_count > 0) m->active_count--;
        }
        pthread_mutex_unlock(&t->mtx);
    }
    int still_active = (m->active_count > 0);
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mtx);
    if (still_active || m->owner_task != NULL) {
        check_quiescence_tree_under_qmtx(m);
    }
    pthread_mutex_unlock(&g_quiesce_mtx);
}

/* =========================================================================
 * Task Creation, Activation, Completion, Abort, Attributes
 * ========================================================================= */

void *__ada_task_create(void *master_ptr,
                        void (*body_fn)(void *, void *),
                        void *env,
                        int32_t num_entries) {
    AdaMaster *m = (AdaMaster *)master_ptr;
    if (!m) m = (AdaMaster *)__ada_global_master();

    AdaTask *t = (AdaTask *)calloc(1, sizeof(AdaTask));
    if (!t) __ada_raise(__ada_exc_storage_error);

    pthread_mutex_init(&t->mtx, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->state = TS_CREATED;
    t->is_callable = 1;
    t->is_terminated = 0;
    t->body_fn = body_fn;
    t->env = env;
    t->num_entries = (num_entries > 0) ? num_entries : 1;
    t->entries = (AdaEntryQueue *)calloc((size_t)t->num_entries, sizeof(AdaEntryQueue));
    t->open_accept = (uint8_t *)calloc((size_t)t->num_entries, sizeof(uint8_t));
    if (!t->entries || !t->open_accept) {
        free(t->entries);
        free(t->open_accept);
        free(t);
        __ada_raise(__ada_exc_storage_error);
    }
    t->master = m;
    if (g_alloc_sp > 0 && g_alloc_stack[g_alloc_sp - 1].master == m) {
        t->group_id = g_alloc_stack[g_alloc_sp - 1].gid;
    }

    pthread_mutex_lock(&m->mtx);
    t->next_in_master = m->tasks;
    m->tasks = t;
    m->total_tasks++;
    m->active_count++;
    pthread_mutex_unlock(&m->mtx);

    return t;
}

/* Create and immediately activate a single task (used for `new Task_Type` allocators, LRM 9.3(4)) */
void *__ada_task_create_and_activate(void *master_ptr,
                                     void (*body_fn)(void *, void *),
                                     void *env,
                                     int32_t num_entries) {
    __ada_check_abort();
    AdaTask *t = (AdaTask *)__ada_task_create(master_ptr, body_fn, env, num_entries);
    AdaMaster *m = t->master;
    AdaActGroup grp = { 1, 0 };
    pthread_mutex_lock(&m->mtx);
    pthread_mutex_lock(&t->mtx);
    t->state = TS_ACTIVATING;
    t->activating = 1;
    t->act_group = &grp;
    t->thread_started = 1;
    if (spawn_task_thread(t) != 0) {
        t->thread_started = 0;
        t->activating = 0;
        t->act_group = NULL;
        grp.pending = 0;
        grp.errors = 1;
        t->state = TS_TERMINATED;
        t->is_callable = 0;
        t->is_terminated = 1;
        cancel_all_queued_calls(t);
        m->activated_done++;
        if (m->active_count > 0) m->active_count--;
    }
    pthread_mutex_unlock(&t->mtx);
    while (grp.pending > 0) {
        pthread_cond_wait(&m->cv, &m->mtx);
    }
    int had_err = (grp.errors > 0);
    pthread_mutex_unlock(&m->mtx);
    __ada_check_abort();
    if (had_err) {
        __ada_raise(__ada_exc_tasking_error);
    }
    return t;
}

void __ada_task_activated(void *self_ptr) {
    AdaTask *t = (AdaTask *)self_ptr;
    if (!t) return;
    pthread_mutex_lock(&t->mtx);
    int was_act = t->activating;
    t->activating = 0;
    if (t->state == TS_ACTIVATING) {
        t->state = TS_RUNNING;
    }
    int ab = __atomic_load_n(&t->aborted, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(&t->mtx);

    AdaMaster *m = t->master;
    if (m && was_act) {
        pthread_mutex_lock(&m->mtx);
        task_account_activation_locked(m, t, 0);
        pthread_mutex_unlock(&m->mtx);
    }
    if (ab && t == g_current_task) {
        abort_exit_self(t);
    }
}

void __ada_task_activation_failed(void *self_ptr) {
    AdaTask *t = (AdaTask *)self_ptr;
    if (!t) return;
    pthread_mutex_lock(&t->mtx);
    int was_act = t->activating;
    t->activating = 0;
    t->state = TS_ACTIVATION_FAILED;
    t->is_callable = 0;
    /* LRM 9.3(5): completed immediately, and terminated immediately only if no child masters */
    if (t->child_masters == NULL) {
        t->is_terminated = 1;
    }
    cancel_all_queued_calls(t);
    pthread_mutex_unlock(&t->mtx);

    AdaMaster *m = t->master;
    if (m && was_act) {
        pthread_mutex_lock(&m->mtx);
        task_account_activation_locked(m, t, 1);
        pthread_mutex_unlock(&m->mtx);
    }
}

static int unlink_call_locked(AdaTask *t, int32_t entry_idx, AdaEntryCall *target);

void __ada_task_completed(void *self_ptr) {
    AdaTask *t = (AdaTask *)self_ptr;
    if (!t) t = g_current_task;
    if (!t) return;
    pthread_mutex_lock(&t->mtx);
    t->is_callable = 0;
    if (t->state == TS_RUNNING || t->state == TS_ACTIVATING) {
        t->state = TS_COMPLETED;
    }
    cancel_all_queued_calls(t);
    AdaMaster *m = t->master;
    pthread_mutex_unlock(&t->mtx);
    if (m) check_quiescence_tree(m);
}

static void ada_task_abort_impl(AdaTask *t, int *self_aborted) {
    if (!t) return;
    AdaTask *callee = NULL;
    int32_t c_entry = 0;
    AdaEntryCall *c_call = NULL;
    int was_created = 0;
    AdaTask *deps_stack[64];
    AdaTask **deps = deps_stack;
    int n_deps = 0;
    int cap_deps = 64;

    pthread_mutex_lock(&t->mtx);
    if (!t->is_terminated) {
        __atomic_store_n(&t->aborted, 1, __ATOMIC_RELEASE);
        callee = t->waiting_callee;
        c_entry = t->waiting_entry;
        c_call = t->waiting_call;
        if (c_call) entry_call_retain(c_call);
    }
    pthread_mutex_unlock(&t->mtx);

    /* Unlink t from any entry queue BEFORE marking is_callable=0 so observers
       seeing T'CALLABLE=FALSE or TASKING_ERROR already see E'COUNT decremented (c9a007a)! */
    if (callee && callee != t && c_call) {
        pthread_mutex_lock(&callee->mtx);
        if (c_call->state == EC_WAITING && unlink_call_locked(callee, c_entry, c_call)) {
            c_call->state = EC_CANCELLED;
            entry_call_release(c_call);
            pthread_cond_broadcast(&callee->cv);
        }
        pthread_mutex_unlock(&callee->mtx);
    }
    if (c_call) {
        entry_call_release(c_call);
        c_call = NULL;
        callee = NULL;
    }

    pthread_mutex_lock(&t->mtx);
    if (!t->is_terminated) {
        __atomic_store_n(&t->aborted, 1, __ATOMIC_RELEASE);
        t->is_callable = 0;
        if (t->state == TS_CREATED) {
            was_created = 1;
            t->state = TS_TERMINATED;
            t->is_terminated = 1;
        } else {
            t->state = TS_ABNORMAL;
        }
        callee = t->waiting_callee;
        c_entry = t->waiting_entry;
        c_call = t->waiting_call;
        if (c_call) entry_call_retain(c_call);
        /* Do NOT cancel active_calls while acceptor may still be executing an accept body!
           Active calls are cancelled safely at __ada_accept_end or abort_exit_self / task_finish_cleanup. */
        cancel_queued_calls_only(t);
        pthread_cond_broadcast(&t->cv);
    }
    /* Snapshot ALL dependent tasks across child masters dynamically (P3-4.13, LRM 9.10(4)) */
    for (AdaMaster *cm = t->child_masters; cm != NULL; cm = cm->next_child) {
        pthread_mutex_lock(&cm->mtx);
        for (AdaTask *dt = cm->tasks; dt != NULL; dt = dt->next_in_master) {
            if (dt != t) {
                if (n_deps >= cap_deps) {
                    int ncap = cap_deps * 2;
                    AdaTask **nd = (deps == deps_stack)
                        ? (AdaTask **)malloc((size_t)ncap * sizeof(AdaTask *))
                        : (AdaTask **)realloc(deps, (size_t)ncap * sizeof(AdaTask *));
                    if (nd) {
                        if (deps == deps_stack) {
                            memcpy(nd, deps_stack, (size_t)n_deps * sizeof(AdaTask *));
                        }
                        deps = nd;
                        cap_deps = ncap;
                    }
                }
                if (n_deps < cap_deps) {
                    deps[n_deps++] = dt;
                }
            }
        }
        pthread_mutex_unlock(&cm->mtx);
    }
    pthread_mutex_unlock(&t->mtx);

    /* Unlink t from any entry queue BEFORE aborting dependents (a dependent may be g_current_task: c9a007a) */
    if (callee && callee != t && c_call) {
        pthread_mutex_lock(&callee->mtx);
        if (c_call->state == EC_WAITING && unlink_call_locked(callee, c_entry, c_call)) {
            c_call->state = EC_CANCELLED;
            entry_call_release(c_call);
            pthread_cond_broadcast(&callee->cv);
        }
        pthread_mutex_unlock(&callee->mtx);
    }
    if (c_call) entry_call_release(c_call);

    if (was_created && t->master) {
        /* P3-4.5: Hold g_quiesce_mtx across active_count decrement & quiescence check to prevent UAF! */
        AdaMaster *m = t->master;
        pthread_mutex_lock(&g_quiesce_mtx);
        pthread_mutex_lock(&m->mtx);
        task_account_activation_locked(m, t, 0);
        if (m->active_count > 0) m->active_count--;
        int still_active = (m->active_count > 0);
        pthread_cond_broadcast(&m->cv);
        pthread_mutex_unlock(&m->mtx);
        if (still_active || m->owner_task != NULL) {
            check_quiescence_tree_under_qmtx(m);
        }
        pthread_mutex_unlock(&g_quiesce_mtx);
    }

    /* Recursively abort dependent tasks */
    for (int i = 0; i < n_deps; ++i) {
        ada_task_abort_impl(deps[i], self_aborted);
    }
    if (deps != deps_stack) {
        free(deps);
    }

    if (t == g_current_task) {
        *self_aborted = 1;
    }
}

void __ada_task_abort(void *task_ptr) {
    __ada_check_abort();
    AdaTask *t = (AdaTask *)task_ptr;
    if (!t) return;
    int self_aborted = 0;
    ada_task_abort_impl(t, &self_aborted);
    /* If the calling task aborted itself (directly or via an enclosing master), terminate now (LRM 9.10(6)) */
    if ((self_aborted || (g_current_task && __atomic_load_n(&g_current_task->aborted, __ATOMIC_ACQUIRE)))
        && g_current_task) {
        abort_exit_self(g_current_task);
    }
}

void __ada_task_abort_list(void **tasks, int32_t count) {
    __ada_check_abort();
    if (!tasks || count <= 0) return;
    int self_aborted = 0;
    for (int32_t i = 0; i < count; ++i) {
        if (tasks[i] != NULL) {
            ada_task_abort_impl((AdaTask *)tasks[i], &self_aborted);
        }
    }
    if ((self_aborted || (g_current_task && __atomic_load_n(&g_current_task->aborted, __ATOMIC_ACQUIRE)))
        && g_current_task) {
        abort_exit_self(g_current_task);
    }
}

int8_t __ada_task_callable(void *task_ptr) {
    AdaTask *t = (AdaTask *)task_ptr;
    if (!t) return 0;
    pthread_mutex_lock(&t->mtx);
    int8_t r = (int8_t)(t->is_callable ? 1 : 0);
    pthread_mutex_unlock(&t->mtx);
    return r;
}

int8_t __ada_task_terminated(void *task_ptr) {
    AdaTask *t = (AdaTask *)task_ptr;
    if (!t) return 1;
    pthread_mutex_lock(&t->mtx);
    int8_t r = (int8_t)(t->is_terminated ? 1 : 0);
    pthread_mutex_unlock(&t->mtx);
    return r;
}

int32_t __ada_entry_count(void *task_ptr, int32_t entry_idx) {
    AdaTask *t = (AdaTask *)task_ptr;
    if (!t || entry_idx < 0 || entry_idx >= t->num_entries) return 0;
    pthread_mutex_lock(&t->mtx);
    int32_t c = t->entries[entry_idx].count;
    pthread_mutex_unlock(&t->mtx);
    return c;
}

/* =========================================================================
 * Delay Statement (LRM 9.6)
 * ========================================================================= */

void __ada_delay(int64_t duration_ns) {
    duration_ns = clamp_delay_ns(duration_ns);
    AdaTask *self = g_current_task;
    if (self) {
        pthread_mutex_lock(&self->mtx);
        if (__atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) {
            pthread_mutex_unlock(&self->mtx);
            abort_exit_self(self);
        }
        if (duration_ns > 0) {
            int64_t end_mono = get_monotonic_ns() + duration_ns;
            while (!__atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) {
                int64_t rem_ns = end_mono - get_monotonic_ns();
                if (rem_ns <= 0) break;
                struct timespec deadline;
                get_realtime_deadline(&deadline, rem_ns);
                (void)pthread_cond_timedwait(&self->cv, &self->mtx, &deadline);
            }
            if (__atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) {
                pthread_mutex_unlock(&self->mtx);
                abort_exit_self(self);
            }
        }
        pthread_mutex_unlock(&self->mtx);
        return;
    }
    if (duration_ns <= 0) return;
    int64_t end_mono = get_monotonic_ns() + duration_ns;
    for (;;) {
        int64_t rem_ns = end_mono - get_monotonic_ns();
        if (rem_ns <= 0) break;
        struct timespec req;
        req.tv_sec = (time_t)(rem_ns / 1000000000LL);
        req.tv_nsec = (long)(rem_ns % 1000000000LL);
        struct timespec rem;
        if (nanosleep(&req, &rem) == 0) break;
        if (errno != EINTR) break;
    }
}

/* =========================================================================
 * Entry Calls & Rendezvous (LRM 9.5, 9.7.2, 9.7.3)
 * ========================================================================= */

static void enqueue_call_locked(AdaTask *t, int32_t entry_idx, AdaEntryCall *c) {
    entry_call_retain(c);
    AdaEntryQueue *q = &t->entries[entry_idx];
    c->next = NULL;
    if (q->tail) {
        q->tail->next = c;
    } else {
        q->head = c;
    }
    q->tail = c;
    q->count++;
}

static int unlink_call_locked(AdaTask *t, int32_t entry_idx, AdaEntryCall *target) {
    AdaEntryQueue *q = &t->entries[entry_idx];
    AdaEntryCall *prev = NULL;
    for (AdaEntryCall *cur = q->head; cur != NULL; prev = cur, cur = cur->next) {
        if (cur == target) {
            if (prev) prev->next = cur->next;
            else q->head = cur->next;
            if (q->tail == cur) q->tail = prev;
            cur->next = NULL;
            q->count--;
            return 1;
        }
    }
    return 0;
}

static AdaEntryCall *dequeue_call_locked(AdaTask *t, int32_t entry_idx) {
    AdaEntryQueue *q = &t->entries[entry_idx];
    AdaEntryCall *c = q->head;
    if (!c) return NULL;
    q->head = c->next;
    if (!q->head) q->tail = NULL;
    c->next = NULL;
    q->count--;
    return c;
}

void __ada_entry_call(void *target_ptr, int32_t entry_idx, void *params) {
    AdaTask *self = g_current_task;
    if (self && __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) abort_exit_self(self);

    AdaTask *t = (AdaTask *)target_ptr;
    if (!t || entry_idx < 0 || entry_idx >= t->num_entries) {
        __ada_raise(__ada_exc_tasking_error);
    }

    AdaEntryCall *call = entry_call_alloc(entry_idx, params, 0);

    if (self) {
        pthread_mutex_lock(&self->mtx);
        if (__atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) {
            pthread_mutex_unlock(&self->mtx);
            entry_call_release(call);
            abort_exit_self(self);
        }
        self->waiting_callee = t;
        self->waiting_entry = entry_idx;
        self->waiting_call = call;
        pthread_mutex_unlock(&self->mtx);
    }

    pthread_mutex_lock(&t->mtx);
    if (self && __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&t->mtx);
        pthread_mutex_lock(&self->mtx);
        self->waiting_callee = NULL;
        self->waiting_call = NULL;
        pthread_mutex_unlock(&self->mtx);
        entry_call_release(call);
        abort_exit_self(self);
    }
    if (!t->is_callable) {
        pthread_mutex_unlock(&t->mtx);
        if (self) {
            pthread_mutex_lock(&self->mtx);
            self->waiting_callee = NULL;
            self->waiting_call = NULL;
            pthread_mutex_unlock(&self->mtx);
        }
        entry_call_release(call);
        __ada_raise(__ada_exc_tasking_error);
    }
    enqueue_call_locked(t, entry_idx, call);
    pthread_cond_broadcast(&t->cv);

    while (call->state == EC_WAITING || call->state == EC_ACCEPTED) {
        pthread_cond_wait(&t->cv, &t->mtx);
    }
    int final_state = call->state;
    const void *exc = call->raised_exc;
    pthread_mutex_unlock(&t->mtx);

    if (self) {
        pthread_mutex_lock(&self->mtx);
        self->waiting_callee = NULL;
        self->waiting_call = NULL;
        int ab = __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE);
        pthread_mutex_unlock(&self->mtx);
        entry_call_release(call);
        if (ab) abort_exit_self(self);
    } else {
        entry_call_release(call);
    }

    if (final_state == EC_TASKING_ERROR) {
        __ada_raise(__ada_exc_tasking_error);
    }
    if (exc != NULL) {
        __ada_raise(exc);
    }
}

int8_t __ada_entry_call_cond(void *target_ptr, int32_t entry_idx, void *params) {
    AdaTask *self = g_current_task;
    if (self && __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) abort_exit_self(self);

    AdaTask *t = (AdaTask *)target_ptr;
    if (!t || entry_idx < 0 || entry_idx >= t->num_entries) {
        __ada_raise(__ada_exc_tasking_error);
    }

    AdaEntryCall *call = entry_call_alloc(entry_idx, params, 1);

    if (self) {
        pthread_mutex_lock(&self->mtx);
        if (__atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) {
            pthread_mutex_unlock(&self->mtx);
            entry_call_release(call);
            abort_exit_self(self);
        }
        self->waiting_callee = t;
        self->waiting_entry = entry_idx;
        self->waiting_call = call;
        pthread_mutex_unlock(&self->mtx);
    }

    pthread_mutex_lock(&t->mtx);
    if (self && __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&t->mtx);
        pthread_mutex_lock(&self->mtx);
        self->waiting_callee = NULL;
        self->waiting_call = NULL;
        pthread_mutex_unlock(&self->mtx);
        entry_call_release(call);
        abort_exit_self(self);
    }
    if (!t->is_callable) {
        pthread_mutex_unlock(&t->mtx);
        if (self) {
            pthread_mutex_lock(&self->mtx);
            self->waiting_callee = NULL;
            self->waiting_call = NULL;
            pthread_mutex_unlock(&self->mtx);
        }
        entry_call_release(call);
        __ada_raise(__ada_exc_tasking_error);
    }
    /* Immediate rendezvous possible only if target is waiting on entry_idx and queue is empty */
    if (!t->open_accept[entry_idx] || t->entries[entry_idx].count > 0) {
        pthread_mutex_unlock(&t->mtx);
        if (self) {
            pthread_mutex_lock(&self->mtx);
            self->waiting_callee = NULL;
            self->waiting_call = NULL;
            int ab = __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE);
            pthread_mutex_unlock(&self->mtx);
            entry_call_release(call);
            if (ab) abort_exit_self(self);
        } else {
            entry_call_release(call);
        }
        return 0;
    }
    enqueue_call_locked(t, entry_idx, call);
    pthread_cond_broadcast(&t->cv);

    while (call->state == EC_WAITING || call->state == EC_ACCEPTED) {
        pthread_cond_wait(&t->cv, &t->mtx);
    }
    int final_state = call->state;
    const void *exc = call->raised_exc;
    pthread_mutex_unlock(&t->mtx);

    if (self) {
        pthread_mutex_lock(&self->mtx);
        self->waiting_callee = NULL;
        self->waiting_call = NULL;
        int ab = __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE);
        pthread_mutex_unlock(&self->mtx);
        entry_call_release(call);
        if (ab) abort_exit_self(self);
    } else {
        entry_call_release(call);
    }

    if (final_state == EC_CANCELLED) {
        return 0;
    }
    if (final_state == EC_TASKING_ERROR) {
        __ada_raise(__ada_exc_tasking_error);
    }
    if (exc != NULL) {
        __ada_raise(exc);
    }
    return 1;
}

int8_t __ada_entry_call_timed(void *target_ptr, int32_t entry_idx, void *params, int64_t delay_ns) {
    if (delay_ns <= 0) {
        return __ada_entry_call_cond(target_ptr, entry_idx, params);
    }
    delay_ns = clamp_delay_ns(delay_ns);
    AdaTask *self = g_current_task;
    if (self && __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) abort_exit_self(self);

    AdaTask *t = (AdaTask *)target_ptr;
    if (!t || entry_idx < 0 || entry_idx >= t->num_entries) {
        __ada_raise(__ada_exc_tasking_error);
    }

    AdaEntryCall *call = entry_call_alloc(entry_idx, params, 0);
    int64_t end_mono = get_monotonic_ns() + delay_ns;

    if (self) {
        pthread_mutex_lock(&self->mtx);
        if (__atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) {
            pthread_mutex_unlock(&self->mtx);
            entry_call_release(call);
            abort_exit_self(self);
        }
        self->waiting_callee = t;
        self->waiting_entry = entry_idx;
        self->waiting_call = call;
        pthread_mutex_unlock(&self->mtx);
    }

    pthread_mutex_lock(&t->mtx);
    if (self && __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&t->mtx);
        pthread_mutex_lock(&self->mtx);
        self->waiting_callee = NULL;
        self->waiting_call = NULL;
        pthread_mutex_unlock(&self->mtx);
        entry_call_release(call);
        abort_exit_self(self);
    }
    if (!t->is_callable) {
        pthread_mutex_unlock(&t->mtx);
        if (self) {
            pthread_mutex_lock(&self->mtx);
            self->waiting_callee = NULL;
            self->waiting_call = NULL;
            pthread_mutex_unlock(&self->mtx);
        }
        entry_call_release(call);
        __ada_raise(__ada_exc_tasking_error);
    }
    enqueue_call_locked(t, entry_idx, call);
    pthread_cond_broadcast(&t->cv);

    while (call->state == EC_WAITING) {
        int64_t rem_ns = end_mono - get_monotonic_ns();
        if (rem_ns <= 0) {
            if (unlink_call_locked(t, entry_idx, call)) {
                call->state = EC_CANCELLED;
                entry_call_release(call);
                pthread_mutex_unlock(&t->mtx);
                if (self) {
                    pthread_mutex_lock(&self->mtx);
                    self->waiting_callee = NULL;
                    self->waiting_call = NULL;
                    int ab = __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE);
                    pthread_mutex_unlock(&self->mtx);
                    entry_call_release(call);
                    if (ab) abort_exit_self(self);
                } else {
                    entry_call_release(call);
                }
                return 0;
            }
            break;
        }
        struct timespec deadline;
        get_realtime_deadline(&deadline, rem_ns);
        int rc = pthread_cond_timedwait(&t->cv, &t->mtx, &deadline);
        if (rc == ETIMEDOUT && call->state == EC_WAITING && get_monotonic_ns() >= end_mono) {
            if (unlink_call_locked(t, entry_idx, call)) {
                call->state = EC_CANCELLED;
                entry_call_release(call);
                pthread_mutex_unlock(&t->mtx);
                if (self) {
                    pthread_mutex_lock(&self->mtx);
                    self->waiting_callee = NULL;
                    self->waiting_call = NULL;
                    int ab = __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE);
                    pthread_mutex_unlock(&self->mtx);
                    entry_call_release(call);
                    if (ab) abort_exit_self(self);
                } else {
                    entry_call_release(call);
                }
                return 0;
            }
        }
    }
    while (call->state == EC_ACCEPTED) {
        pthread_cond_wait(&t->cv, &t->mtx);
    }
    int final_state = call->state;
    const void *exc = call->raised_exc;
    pthread_mutex_unlock(&t->mtx);

    if (self) {
        pthread_mutex_lock(&self->mtx);
        self->waiting_callee = NULL;
        self->waiting_call = NULL;
        int ab = __atomic_load_n(&self->aborted, __ATOMIC_ACQUIRE);
        pthread_mutex_unlock(&self->mtx);
        entry_call_release(call);
        if (ab) abort_exit_self(self);
    } else {
        entry_call_release(call);
    }

    if (final_state == EC_TASKING_ERROR) {
        __ada_raise(__ada_exc_tasking_error);
    }
    if (exc != NULL) {
        __ada_raise(exc);
    }
    return 1;
}

/* =========================================================================
 * Accept & Selective Wait (LRM 9.5, 9.7.1)
 * ========================================================================= */

static void rendezvous_inherit_priority_locked(AdaTask *t, AdaEntryCall *c) {
    /* LRM 9.8(4) (P3-4.14): rendezvous executes at max(caller_prio, acceptor_prio) */
    c->saved_acc_has_prio = t->has_priority;
    c->saved_acc_prio = t->priority;
    if (c->caller_prio >= 0 && (!t->has_priority || c->caller_prio > t->priority)) {
        t->has_priority = 1;
        t->priority = c->caller_prio;
        apply_thread_priority(pthread_self(), t->priority);
    }
}

void *__ada_accept_begin(void *self_ptr, int32_t entry_idx, void **params_out) {
    AdaTask *t = (AdaTask *)self_ptr;
    if (!t) t = g_current_task;
    if (!t || entry_idx < 0 || entry_idx >= t->num_entries) {
        __ada_raise(__ada_exc_program_error);
    }

    pthread_mutex_lock(&t->mtx);
    t->open_accept[entry_idx] = 1;
    while (t->entries[entry_idx].count == 0 && !t->aborted) {
        pthread_cond_wait(&t->cv, &t->mtx);
    }
    t->open_accept[entry_idx] = 0;
    if (t->aborted) {
        pthread_mutex_unlock(&t->mtx);
        abort_exit_self(t);
    }
    AdaEntryCall *c = dequeue_call_locked(t, entry_idx);
    if (c) {
        c->state = EC_ACCEPTED;
        c->prev_active = t->active_calls;
        t->active_calls = c;
        rendezvous_inherit_priority_locked(t, c);
        if (params_out) *params_out = c->params;
    }
    pthread_mutex_unlock(&t->mtx);
    return c;
}

void __ada_accept_end(void *self_ptr, void *call_ptr, const void *raised_exc) {
    AdaTask *t = (AdaTask *)self_ptr;
    if (!t) t = g_current_task;
    AdaEntryCall *c = (AdaEntryCall *)call_ptr;
    if (!t || !c) return;

    pthread_mutex_lock(&t->mtx);
    int found = 0;
    AdaEntryCall **pp = &t->active_calls;
    while (*pp) {
        if (*pp == c) {
            *pp = c->prev_active;
            found = 1;
            break;
        }
        pp = &(*pp)->prev_active;
    }
    int ab = __atomic_load_n(&t->aborted, __ATOMIC_ACQUIRE);
    if (found) {
        /* Restore acceptor's priority after rendezvous (LRM 9.8(4)) */
        int prev_hp = t->has_priority;
        int32_t prev_p = t->priority;
        t->has_priority = c->saved_acc_has_prio;
        t->priority = c->saved_acc_prio;
        if (t->has_priority && (!prev_hp || t->priority != prev_p)) {
            apply_thread_priority(pthread_self(), t->priority);
        }
        c->prev_active = NULL;
        if (ab && raised_exc == NULL) {
            c->raised_exc = NULL;
            c->state = EC_TASKING_ERROR;
        } else {
            c->raised_exc = raised_exc;
            c->state = EC_DONE;
        }
        entry_call_release(c);
    }
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mtx);
    /* LRM 9.10(6): end of accept statement is a mandatory abort synchronization point */
    if (ab && t == g_current_task) {
        abort_exit_self(t);
    }
}

/*
 * Selective wait:
 *   Returns:
 *     >= 0 : index of chosen accept or delay alternative (0..n_alts-1)
 *     -1   : else alternative chosen
 *     -2   : terminate alternative chosen (caller should exit task body cleanly)
 */
int32_t __ada_select_wait(void *self_ptr,
                          int32_t n_alts,
                          const int32_t *alt_kinds,
                          const int32_t *entry_indices,
                          const int64_t *delays_ns,
                          int32_t has_else,
                          void **params_out,
                          void **call_out) {
    AdaTask *t = (AdaTask *)self_ptr;
    if (!t) t = g_current_task;
    if (!t) __ada_raise(__ada_exc_program_error);

    /* P3-4.13: Check abort synchronization point BEFORE raising PROGRAM_ERROR on all-closed select! */
    if (__atomic_load_n(&t->aborted, __ATOMIC_ACQUIRE)) {
        abort_exit_self(t);
    }

    int has_open = 0;
    int has_open_delay = 0;
    int32_t best_delay_alt = -1;
    int64_t best_delay_ns = 0;
    int has_open_terminate = 0;

    for (int32_t i = 0; i < n_alts; ++i) {
        if (alt_kinds[i] == ALT_ACCEPT) {
            has_open = 1;
        } else if (alt_kinds[i] == ALT_DELAY) {
            has_open = 1;
            if (!has_open_delay || delays_ns[i] < best_delay_ns) {
                has_open_delay = 1;
                best_delay_ns = delays_ns[i];
                best_delay_alt = i;
            }
        } else if (alt_kinds[i] == ALT_TERMINATE) {
            has_open = 1;
            has_open_terminate = 1;
        }
    }

    if (!has_open && !has_else) {
        __ada_raise(__ada_exc_program_error);
    }

    pthread_mutex_lock(&t->mtx);
    /* LRM 9.10(6): selective wait entry is a mandatory abort synchronization point */
    if (__atomic_load_n(&t->aborted, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&t->mtx);
        abort_exit_self(t);
    }

    /* 1. Check for any immediately ready open accept */
    for (int32_t i = 0; i < n_alts; ++i) {
        if (alt_kinds[i] == ALT_ACCEPT) {
            int32_t eidx = entry_indices[i];
            if (eidx >= 0 && eidx < t->num_entries && t->entries[eidx].count > 0) {
                AdaEntryCall *c = dequeue_call_locked(t, eidx);
                c->state = EC_ACCEPTED;
                c->prev_active = t->active_calls;
                t->active_calls = c;
                rendezvous_inherit_priority_locked(t, c);
                if (params_out) *params_out = c->params;
                if (call_out) *call_out = c;
                pthread_mutex_unlock(&t->mtx);
                return i;
            }
        }
    }

    /* 2. If else branch exists, take it immediately */
    if (has_else) {
        pthread_mutex_unlock(&t->mtx);
        return -1;
    }

    /* 3. If best delay <= 0, take delay alternative immediately */
    if (has_open_delay && best_delay_ns <= 0) {
        pthread_mutex_unlock(&t->mtx);
        return best_delay_alt;
    }

    /* 4. Mark all open accept entries so conditional entry calls can rendezvous */
    for (int32_t i = 0; i < n_alts; ++i) {
        if (alt_kinds[i] == ALT_ACCEPT) {
            int32_t eidx = entry_indices[i];
            if (eidx >= 0 && eidx < t->num_entries) {
                t->open_accept[eidx] = 1;
            }
        }
    }

    int64_t end_mono = 0;
    if (has_open_delay) {
        end_mono = get_monotonic_ns() + clamp_delay_ns(best_delay_ns);
    }

    AdaMaster *m = t->master;
    if (has_open_terminate && m) {
        t->waiting_terminate = 1;
        pthread_mutex_unlock(&t->mtx);
        pthread_mutex_lock(&m->mtx);
        m->waiting_terminate++;
        pthread_mutex_unlock(&m->mtx);
        check_quiescence_tree(m);
        pthread_mutex_lock(&t->mtx);
    }

    int32_t chosen = -2;
    int was_aborted = 0;
    for (;;) {
        if (__atomic_load_n(&t->aborted, __ATOMIC_ACQUIRE)) {
            was_aborted = 1;
            chosen = -2;
            break;
        }
        /* P3-4.4: Once quiescence atomically committed termination on t under t->mtx, exit immediately! */
        if (has_open_terminate && t->terminate_committed) {
            chosen = -2;
            break;
        }
        /* Check open accept queues */
        int found = -1;
        for (int32_t i = 0; i < n_alts; ++i) {
            if (alt_kinds[i] == ALT_ACCEPT) {
                int32_t eidx = entry_indices[i];
                if (eidx >= 0 && eidx < t->num_entries && t->entries[eidx].count > 0) {
                    found = i;
                    break;
                }
            }
        }
        if (found >= 0) {
            int32_t eidx = entry_indices[found];
            AdaEntryCall *c = dequeue_call_locked(t, eidx);
            c->state = EC_ACCEPTED;
            c->prev_active = t->active_calls;
            t->active_calls = c;
            rendezvous_inherit_priority_locked(t, c);
            if (params_out) *params_out = c->params;
            if (call_out) *call_out = c;
            chosen = found;
            break;
        }
        if (has_open_delay) {
            int64_t rem_ns = end_mono - get_monotonic_ns();
            if (rem_ns <= 0) {
                chosen = best_delay_alt;
                break;
            }
            struct timespec deadline;
            get_realtime_deadline(&deadline, rem_ns);
            int rc = pthread_cond_timedwait(&t->cv, &t->mtx, &deadline);
            if (rc == ETIMEDOUT && get_monotonic_ns() >= end_mono) {
                chosen = best_delay_alt;
                break;
            }
        } else {
            pthread_cond_wait(&t->cv, &t->mtx);
        }
    }

    int cancelled_any_cond = 0;
    for (int32_t i = 0; i < n_alts; ++i) {
        if (alt_kinds[i] == ALT_ACCEPT) {
            int32_t eidx = entry_indices[i];
            if (eidx >= 0 && eidx < t->num_entries) {
                t->open_accept[eidx] = 0;
                /* Cancel any conditional calls left queued on unchosen arms so they take else branch */
                AdaEntryCall *cur = t->entries[eidx].head;
                while (cur) {
                    AdaEntryCall *nxt = cur->next;
                    if (cur->is_conditional && cur->state == EC_WAITING) {
                        if (unlink_call_locked(t, eidx, cur)) {
                            cur->state = EC_CANCELLED;
                            entry_call_release(cur);
                            cancelled_any_cond = 1;
                        }
                    }
                    cur = nxt;
                }
            }
        }
    }
    if (cancelled_any_cond) {
        pthread_cond_broadcast(&t->cv);
    }

    if (has_open_terminate && m && t->waiting_terminate) {
        t->waiting_terminate = 0;
        pthread_mutex_unlock(&t->mtx);
        pthread_mutex_lock(&m->mtx);
        if (m->waiting_terminate > 0) m->waiting_terminate--;
        pthread_mutex_unlock(&m->mtx);
    } else {
        pthread_mutex_unlock(&t->mtx);
    }
    if (was_aborted) {
        abort_exit_self(t);
    }
    return chosen;
}
