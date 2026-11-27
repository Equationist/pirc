/* C17 6.2.7p4: hidden file-scope array bound must not leak through shadowed block-scope extern */
extern int a[3];
void f(void) {
    int a = 5;
    {
        extern int a[];
        int s = sizeof(a);
        (void)s;
    }
    (void)a;
}
