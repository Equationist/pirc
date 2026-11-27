/* C17 6.3.2.1p3 / 6.7.1p6: explicit cast of register array must be diagnosed */
void f(void) {
    register int a[5];
    int *p = (int *)a;
    (void)p;
}
