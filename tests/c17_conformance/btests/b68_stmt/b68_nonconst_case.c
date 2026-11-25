/* C99 §6.8.4.2 p3: Case label expression must be an integer constant expression */
int f(int x, int y) {
    switch (x) {
    case y: return 1;
    default: return 0;
    }
}
