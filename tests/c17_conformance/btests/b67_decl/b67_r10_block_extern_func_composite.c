/* C17 6.2.7p2: incompatible block-scope extern function declarations */
void g(void) {
    extern void f(int (*)[4]);
    extern void f(int (*)[5]);
}
