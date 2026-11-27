/* C17 6.8.6.1p1: goto jumping into scope of block-scope static VM pointer */
void f(int n) {
    goto L;
    static int (*p)[n];
L:
    (void)p;
}
