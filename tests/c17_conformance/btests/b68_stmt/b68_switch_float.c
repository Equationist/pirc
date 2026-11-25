/* C99 §6.8.4.2 p1: Controlling expression of switch must have integer type */
int f(double d) {
    switch (d) {
    case 1: return 1;
    default: return 0;
    }
}
