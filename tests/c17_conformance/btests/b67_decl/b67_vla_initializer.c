/* C99 §6.7.8 p3: Variable-length array cannot have an initializer */
int f(int n) {
    int a[n] = { 0 };
    return a[0];
}
