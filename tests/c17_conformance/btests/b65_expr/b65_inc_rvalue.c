/* C17 §6.5.3.1 p1: Increment operand must be a modifiable lvalue */
int f(int a, int b) {
    return ++(a + b);
}
