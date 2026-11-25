/* C99 §6.5.3.2 p1: Cannot take address of register-qualified variable */
int *f(void) {
    register int r = 42;
    return &r;
}
