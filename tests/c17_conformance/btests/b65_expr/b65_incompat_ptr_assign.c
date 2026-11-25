/* C99 §6.5.16.1: Incompatible pointer assignment without cast */
int *f(double *d) {
    int *p = d;
    return p;
}
