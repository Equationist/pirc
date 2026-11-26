/* ISO C17 §6.2.7p4: block-scope extern array bound must not complete file-scope incomplete type */
extern int a[];

int f(void) {
    extern int a[3];
    return (int)sizeof(a);
}

int bad_sz = (int)sizeof(a);
