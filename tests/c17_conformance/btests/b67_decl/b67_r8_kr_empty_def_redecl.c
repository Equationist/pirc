/* C17 §6.7.6.3p15: empty K&R definition is incompatible with 1-param prototype */
void f() {}
void f(int x);
int main(void) { return 0; }
