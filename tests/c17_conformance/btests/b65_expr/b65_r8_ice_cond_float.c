/* C17 §6.6p6: floating constant operand of !/&&/||/?: is not an ICE */
enum { A = !1.5 };
int main(void) { return A; }
