/* C17 §6.7.4p2: _Noreturn function specifier is invalid in type names */
int a = (int)sizeof(_Noreturn int);
int main(void) { return a; }
