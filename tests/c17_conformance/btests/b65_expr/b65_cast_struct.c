/* C99 §6.5.4 p2: Cannot cast struct type to scalar int */
struct S { int x; };
int f(struct S s) {
    return (int)s;
}
