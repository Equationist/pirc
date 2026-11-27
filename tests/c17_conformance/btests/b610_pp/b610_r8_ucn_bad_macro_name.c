/* C17 §6.4.3p2: invalid UCN (< 0xA0) cannot appear in macro name */
#define a\u0040 42
int main(void) { return a\u0040; }
