/* C17 §6.5.6p3: both operands of pointer subtraction must point to complete object types */
long f(int (*p)[5], int (*q)[]) { return p - q; }
int main(void) { return 0; }
