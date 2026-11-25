/* C17 §6.5.6 p3: Subtracting pointers to incompatible types */
long f(int *a, double *b) {
    return a - b;
}
