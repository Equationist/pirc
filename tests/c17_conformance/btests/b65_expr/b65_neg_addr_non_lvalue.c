/* ISO C17 §6.5.3.2p1 — Operand of unary '&' must be an lvalue or function designator. */
int *f(int x) {
    return &(x + 1);
}
