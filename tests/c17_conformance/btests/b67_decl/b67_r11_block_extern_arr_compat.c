/* C17 6.2.7p2: incompatible block-scope extern arrays after incomplete extern */
void f(void) { extern int a[]; (void)a; }
void g(void) { extern int a[3]; (void)a; }
void h(void) { extern int a[4]; (void)a; }
