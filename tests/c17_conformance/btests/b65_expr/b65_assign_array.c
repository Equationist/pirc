/* C17 §6.5.16 p2: Array is not a modifiable lvalue */
void f(void) {
    int a[2] = { 1, 2 };
    int b[2] = { 3, 4 };
    a = b;
}
