/* C99 §6.5.16.1: Assignment to const-qualified object */
int f(void) {
    const int x = 5;
    x = 6;
    return x;
}
