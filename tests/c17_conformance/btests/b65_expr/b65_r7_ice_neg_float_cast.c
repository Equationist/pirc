/* C17 §6.6p6: unary negation of float constant is not an immediate float operand of cast. */
enum { A = (int)-1.5 };
int main(void) { return A; }
