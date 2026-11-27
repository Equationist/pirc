/* C17 §6.5.16p2: non-lvalue assignment is a constraint violation even inside sizeof */
int x = (int)sizeof(1 = 2);
int main(void) { return x; }
