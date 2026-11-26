/* C17 §6.6p6: manual null-pointer offsetof is not an integer constant expression. */
struct S { int a; int b; };
enum { A = (long)&((struct S *)0)->b };
int main(void) { return A; }
