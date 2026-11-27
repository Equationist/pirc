/* C17 6.7.6.3p15: _Atomic parameter mismatch between prototype and K&R definition */
void f(int x);
void f(x) _Atomic int x; { (void)x; }
