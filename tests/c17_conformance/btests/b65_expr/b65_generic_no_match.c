/* C17 §6.5.1.1 p2: Controlling expression matches no type and no default present */
int f(double x) {
    return _Generic(x, int: 1, long: 2);
}
