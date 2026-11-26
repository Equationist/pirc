/* C helper for Ada 83 pragma INTERFACE (C, ...) regression test (P3-6.9) */
#include <stdint.h>

int32_t p3_c_add(int32_t a, int32_t b) {
    return a + b;
}

double p3_c_mul_f64(double a, double b) {
    return a * b;
}

void p3_c_bump(int32_t *x, int32_t delta) {
    *x += delta;
}

int32_t p3_c_sum_bytes(const char *s, int32_t n) {
    int32_t total = 0;
    for (int32_t i = 0; i < n; i++) {
        total += (unsigned char)s[i];
    }
    return total;
}
