/*
 * C99 §6.7.7 & §6.2.3: Typedef name re-scoping and separate namespaces.
 * A typedef name may be shadowed by an ordinary identifier in an inner scope,
 * by a struct/union member name, by a tag name, or by a label.
 */

typedef int T;

struct T {
    T T; /* member named T of type typedef T */
};

int use_typedef_shadow(T x) {
    struct T s;
    s.T = x;
    {
        /* Ordinary variable shadows typedef T in inner scope */
        int T = 42;
        s.T += T;
    }
    /* Outer typedef T visible again here */
    T y = s.T;
T: /* Label T in label namespace */
    if (y < 0) {
        y = 0;
        goto T;
    }
    return y;
}

/* Parameter name shadowing typedef in prototype (requires explicit type) */
int param_shadow(int T, const int U) {
    return T + U;
}
