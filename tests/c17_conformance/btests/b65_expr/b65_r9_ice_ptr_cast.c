/* C17 §6.6p6: Pointer cast in integer constant expression is disallowed */
int a[(int)(void *)0 + 1];
int main(void) { return 0; }
