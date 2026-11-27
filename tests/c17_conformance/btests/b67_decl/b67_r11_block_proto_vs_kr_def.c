/* C17 6.2.7p2 / 6.7.6.3p15: block-scope prototype vs incompatible K&R definition */
void g(void) { extern int f(int); (void)f; }
int f(a, b) int a; int b; { return a + b; }
