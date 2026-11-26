/* C17 §6.6p6: pointer comparison is not an integer constant expression. */
enum { A = ((void *)0 == (void *)0) };
int main(void) { return A; }
