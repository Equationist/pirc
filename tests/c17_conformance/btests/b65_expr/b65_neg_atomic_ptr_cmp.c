/* ISO C17 §6.5.9p2 — Comparing pointers to compatible types with differing _Atomic qualifier. */
int f(int *p, _Atomic int *q) {
    return p == q;
}
