/* C17 6.7.4p2: block-scope extern inline with no declarator must be diagnosed */
void f(void) {
    extern inline struct S { int a; };
}
