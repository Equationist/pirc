/* ISO C17 §6.4.1 — GNU __auto_type is not allowed in strict ISO C17. */
int f(void) {
    __auto_type x = 1;
    return x;
}
