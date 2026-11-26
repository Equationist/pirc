/* C17 §6.9.1p3: function definition returning qualified void. */
const void f(void) {}
int main(void) { f(); return 0; }
