/* C17 §6.6p4: signed integer overflow in integer constant expression. */
enum { A = 2147483647 + 1 };
int main(void) { return A; }
