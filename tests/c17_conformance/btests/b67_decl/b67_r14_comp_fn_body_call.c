/* C17 6.2.7: Function's composite type must be visible inside its own body. */
void f(int (*p)[2]);
void f(int (*p)[]) {
  int a[3] = {0};
  f(&a);
}
int main(void) { return 0; }
