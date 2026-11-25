/* C99 §6.7.2.1 p8: Struct member cannot have variably modified (VLA) type */
void f(int n) {
    struct S {
        int a[n];
    } s;
    (void)s;
}
