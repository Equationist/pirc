/* C17 6.7.6.3p15: K&R definition parameter type conflicts with subsequent prototype */
void f();
void f(x)
int x;
{ (void)x; }
void f(double x);
