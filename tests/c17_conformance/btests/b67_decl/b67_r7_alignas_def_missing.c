/* C17 §6.7.5p6: definition missing _Alignas from prior declaration. */
extern _Alignas(16) int x;
int x;
int main(void) { return x; }
