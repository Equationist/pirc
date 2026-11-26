/* C17 §6.6p4: out-of-range shift count in integer constant expression. */
enum { A = 1 << 64 };
int main(void) { return A; }
