/* C17 §6.7.5p6: conflicting _Alignas on redeclaration. */
extern _Alignas(16) int x;
_Alignas(32) int x;
int main(void) { return x; }
