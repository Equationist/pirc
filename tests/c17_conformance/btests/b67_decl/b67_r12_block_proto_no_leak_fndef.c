/* C17 6.2.7p4 / 6.9.1p7: out-of-scope block prototype must not complete file-scope fndef return */
void g(void) {
    extern int (*f(void))[3];
}
int (*f(void))[] { return 0; }
int s = sizeof(*f());
