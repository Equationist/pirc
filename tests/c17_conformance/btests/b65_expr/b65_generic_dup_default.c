/* C17 §6.5.1.1 p2: At most one default generic association */
int f(int x) {
    return _Generic(x, default: 1, default: 2);
}
