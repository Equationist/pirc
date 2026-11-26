/* C17 §6.6p6: pointer-to-integer cast is not an integer constant expression. */
enum { A = (int)(void *)0 };
int main(void) { return A; }
