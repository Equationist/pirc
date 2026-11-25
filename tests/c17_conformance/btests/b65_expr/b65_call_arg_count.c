/* C99 §6.5.2.2 p2: Function call with wrong argument count */
int add(int a, int b);
int f(void) {
    return add(1, 2, 3);
}
