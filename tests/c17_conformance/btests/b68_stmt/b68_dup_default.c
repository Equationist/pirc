/* C99 §6.8.4.2 p3: At most one default label in a switch statement */
int f(int x) {
    switch (x) {
    default: return 1;
    default: return 2;
    }
}
