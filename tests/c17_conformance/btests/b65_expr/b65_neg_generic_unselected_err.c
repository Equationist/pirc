/* ISO C17 §6.5.1.1 — Unselected _Generic associations must still satisfy type constraints. */
struct S { int x; };
int f(void) {
    struct S s = { 0 };
    return _Generic(1, int: 1, double: s + 1);
}
