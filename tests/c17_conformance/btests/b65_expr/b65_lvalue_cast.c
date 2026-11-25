/* C99 §6.5.4: Result of a cast is not an lvalue */
int f(int x) {
    (int)x = 5;
    return x;
}
