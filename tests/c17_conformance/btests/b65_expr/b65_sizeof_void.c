/* C99 §6.5.3.4 p1: sizeof cannot be applied to incomplete type void */
unsigned long f(void) {
    return sizeof(void);
}
