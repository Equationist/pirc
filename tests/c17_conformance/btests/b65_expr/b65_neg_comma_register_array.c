/* ISO C17 §6.7.1p6 — Array with register storage class cannot decay to pointer via comma operator. */
int *f(void) {
    register int a[4] = { 1, 2, 3, 4 };
    return (0, a);
}
