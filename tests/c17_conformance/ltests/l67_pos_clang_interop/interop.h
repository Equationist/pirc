#ifndef INTEROP_H
#define INTEROP_H

#include <stdbool.h>

struct InteropHFA { double x, y, z; };
struct InteropLarge { long long v[5]; };

struct InteropHFA clang_add_hfa(struct InteropHFA a, struct InteropHFA b);
struct InteropLarge clang_sum_large(struct InteropLarge a, struct InteropLarge b);
bool clang_negate_bool(bool b);
long long clang_varargs_mix(int count, ...);
int clang_call_cb(int (*cb)(int, double), int a, double b);

#endif
