/* C17 §6.6p6: floating-point comparison is not an integer constant expression. */
enum { A = (1.0 == 1.0) };
int main(void) { return A; }
