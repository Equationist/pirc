/* C17 §6.5.9 p2: Equality comparison '==' is not defined on struct types */
struct S { int x; };
int f(struct S a, struct S b) {
    return a == b;
}
