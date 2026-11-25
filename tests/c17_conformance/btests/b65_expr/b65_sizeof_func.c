/* C99 §6.5.3.4 p1: sizeof cannot be applied to function type */
void g(void);
unsigned long f(void) {
    return sizeof(g);
}
