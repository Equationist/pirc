/* C17 6.7.9p14: _Atomic character array cannot be initialized by a string literal. */
_Atomic char s[] = "abc";
int main(void) { return s[0]; }
