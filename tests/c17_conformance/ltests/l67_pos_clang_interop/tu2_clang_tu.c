/* Cross-compiler ABI interop: TU compiled with host clang -std=c99 */
#include "interop.h"
#include <stdarg.h>

struct InteropHFA clang_add_hfa(struct InteropHFA a, struct InteropHFA b) {
    struct InteropHFA r = { a.x + b.x, a.y + b.y, a.z + b.z };
    return r;
}

struct InteropLarge clang_sum_large(struct InteropLarge a, struct InteropLarge b) {
    struct InteropLarge r;
    for (int i = 0; i < 5; i++) {
        r.v[i] = a.v[i] + b.v[i];
    }
    return r;
}

bool clang_negate_bool(bool b) {
    return !b;
}

long long clang_varargs_mix(int count, ...) {
    va_list ap;
    va_start(ap, count);
    long long s = 0;
    for (int i = 0; i < count; i++) {
        if ((i & 1) == 0) {
            s += va_arg(ap, int);
        } else {
            s += va_arg(ap, long long);
        }
    }
    va_end(ap);
    return s;
}

int clang_call_cb(int (*cb)(int, double), int a, double b) {
    return cb(a, b);
}
