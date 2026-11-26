/* C17 §6.6p6: string literal subscript is not an integer constant expression. */
enum { A = "abc"[0] };
int main(void) { return A; }
