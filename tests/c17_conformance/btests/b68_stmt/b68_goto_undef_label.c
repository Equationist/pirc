/* C17 §6.8.6.1 p1: goto targeting an undefined label */
void f(void) {
    goto nonexistent_label;
}
