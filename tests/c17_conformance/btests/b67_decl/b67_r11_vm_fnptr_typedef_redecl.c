/* C17 6.7p3 / 6.7.6p3: redefining variably-modified function-pointer typedef */
void f(int n, int m) {
    typedef void (*FP)(int (*)[n]);
    typedef void (*FP)(int (*)[m]);
}
