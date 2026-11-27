/* C17 §6.6p6: unevaluated branch of ternary ICE cannot contain float arithmetic */
enum { A = 1 ? 0 : (int)(1.0 + 2.0) };
int main(void) { return A; }
