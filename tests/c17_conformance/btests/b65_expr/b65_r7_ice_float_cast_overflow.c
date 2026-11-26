/* C17 §6.6p4: out-of-range float-to-int cast in integer constant expression. */
enum { A = (int)1e30 };
int main(void) { return A; }
