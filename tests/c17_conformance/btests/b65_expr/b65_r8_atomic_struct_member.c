/* C17 §6.5.2.3p5: member access on _Atomic struct is disallowed */
_Atomic struct T { int x; } t;
int f(void) { return t.x; }
int main(void) { return f(); }
