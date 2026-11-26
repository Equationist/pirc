/* ISO C17 §6.6p6 — Short-circuited integer constant expression must not contain non-constant var. */
int f(int x) {
    enum { E = 0 && x };
    return E;
}
