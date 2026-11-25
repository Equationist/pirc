/* C17-LDFLAGS: -lpthread */
/* C17 §6.2.4: extern _Thread_local across TUs with pthreads (TU 1) */
#include "c17_test.h"
#include <pthread.h>

extern _Thread_local int ext_tls_val;
extern int tu2_add_tls(int d);

struct Arg { int id; int ok; };

static void *tls_worker(void *p) {
    struct Arg *a = (struct Arg *)p;
    int initial_ok = (ext_tls_val == 500);
    ext_tls_val += a->id * 10;
    int after = tu2_add_tls(a->id);
    a->ok = initial_ok && (after == 500 + a->id * 11) && (ext_tls_val == after);
    return NULL;
}

int main(void) {
    C17_BEGIN("l62_pos_tls_extern", "Multi-TU extern _Thread_local isolation with pthreads");
    C17_CHECK_EQ(ext_tls_val, 500);
    ext_tls_val = 900;

    pthread_t th[4];
    struct Arg args[4];
    for (int i = 0; i < 4; i++) {
        args[i].id = i + 1;
        args[i].ok = 0;
        C17_CHECK_EQ(pthread_create(&th[i], NULL, tls_worker, &args[i]), 0);
    }
    for (int i = 0; i < 4; i++) {
        C17_CHECK_EQ(pthread_join(th[i], NULL), 0);
        C17_CHECK_EQ(args[i].ok, 1);
    }
    C17_CHECK_EQ(ext_tls_val, 900);
    C17_CHECK_EQ(tu2_add_tls(5), 905);
    C17_END();
}
