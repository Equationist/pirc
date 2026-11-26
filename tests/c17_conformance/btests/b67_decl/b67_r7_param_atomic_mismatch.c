/* C17 §6.7.6.3p15: _Atomic parameter mismatch between declaration and definition. */
struct S { char c[3]; };
void f(struct S s);
void f(_Atomic struct S s) { (void)s; }
int main(void) { return 0; }
