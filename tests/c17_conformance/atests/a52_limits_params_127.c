/*
 * C99 §5.2.4.1: Translation limits —
 *  - 127 parameters in one function definition and 127 arguments in one call
 *  - 127 parameters in one macro definition
 */

#define P10(p) int p##0, int p##1, int p##2, int p##3, int p##4, \
               int p##5, int p##6, int p##7, int p##8, int p##9
#define A10(p) p##0 + p##1 + p##2 + p##3 + p##4 + \
               p##5 + p##6 + p##7 + p##8 + p##9
#define V10 1, 2, 3, 4, 5, 6, 7, 8, 9, 10

int sum127(
    P10(a), P10(b), P10(c), P10(d), P10(e), P10(f),
    P10(g), P10(h), P10(i), P10(j), P10(k), P10(l),
    int m0, int m1, int m2, int m3, int m4, int m5, int m6
) {
    return A10(a) + A10(b) + A10(c) + A10(d) + A10(e) + A10(f) +
           A10(g) + A10(h) + A10(i) + A10(j) + A10(k) + A10(l) +
           m0 + m1 + m2 + m3 + m4 + m5 + m6;
}

#define M127( \
    a0,a1,a2,a3,a4,a5,a6,a7,a8,a9, \
    b0,b1,b2,b3,b4,b5,b6,b7,b8,b9, \
    c0,c1,c2,c3,c4,c5,c6,c7,c8,c9, \
    d0,d1,d2,d3,d4,d5,d6,d7,d8,d9, \
    e0,e1,e2,e3,e4,e5,e6,e7,e8,e9, \
    f0,f1,f2,f3,f4,f5,f6,f7,f8,f9, \
    g0,g1,g2,g3,g4,g5,g6,g7,g8,g9, \
    h0,h1,h2,h3,h4,h5,h6,h7,h8,h9, \
    i0,i1,i2,i3,i4,i5,i6,i7,i8,i9, \
    j0,j1,j2,j3,j4,j5,j6,j7,j8,j9, \
    k0,k1,k2,k3,k4,k5,k6,k7,k8,k9, \
    l0,l1,l2,l3,l4,l5,l6,l7,l8,l9, \
    m0,m1,m2,m3,m4,m5,m6) ((a0) + (m6))

int call_sum127(void) {
    int x = M127(
        1,2,3,4,5,6,7,8,9,10, 1,2,3,4,5,6,7,8,9,10,
        1,2,3,4,5,6,7,8,9,10, 1,2,3,4,5,6,7,8,9,10,
        1,2,3,4,5,6,7,8,9,10, 1,2,3,4,5,6,7,8,9,10,
        1,2,3,4,5,6,7,8,9,10, 1,2,3,4,5,6,7,8,9,10,
        1,2,3,4,5,6,7,8,9,10, 1,2,3,4,5,6,7,8,9,10,
        1,2,3,4,5,6,7,8,9,10, 1,2,3,4,5,6,7,8,9,10,
        1,2,3,4,5,6,7);
    return x + sum127(V10, V10, V10, V10, V10, V10,
                      V10, V10, V10, V10, V10, V10,
                      1, 2, 3, 4, 5, 6, 7);
}
