/* C99 §6.7.3 p2: restrict qualifier may only be applied to pointer types */
int f(void) {
    restrict int x = 5;
    return x;
}
