/* C99 §6.7 p3: Duplicate declaration of variable in same block scope */
int f(void) {
    int x = 1;
    int x = 2;
    return x;
}
