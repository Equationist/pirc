/* C17 §6.8.1 p3: Duplicate label name in the same function */
void f(void) {
dup:
    ;
dup:
    ;
}
