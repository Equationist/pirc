/* C17 §6.5.4p2: cast operator requires scalar type (except void) */
struct S { int x; };
struct S f(struct S s) {
    return (struct S)s;
}
