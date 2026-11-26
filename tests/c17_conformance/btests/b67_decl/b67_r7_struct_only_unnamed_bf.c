/* C17 §6.7.2.1p8: struct with only unnamed bit-field is rejected under -pedantic-errors. */
struct S { int : 3; };
int main(void) { return 0; }
