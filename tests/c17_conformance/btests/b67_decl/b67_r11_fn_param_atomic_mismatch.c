/* C17 6.7.6.3p15 / DR 423: _Atomic qualifier mismatch on function parameter */
void f(int x);
void f(_Atomic int x);
