/* C17 6.2.7p3 / 6.5.3.4p2: composite of VLA int[2][n] and int[][3] stays VLA */
void f(int n, int (*p)[2][n], int (*q)[][3]) {
    _Static_assert(sizeof(*(1 ? p : q)) == 24, "vla");
}
