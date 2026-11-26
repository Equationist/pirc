/* C17 §6.7.6.3p15: K&R function definition followed by incompatible prototype. */
void f(a) int a; { (void)a; }
void f(double a);
int main(void) { return 0; }
