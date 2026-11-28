/* C17 6.6 / 6.7.6.2: Signed integer overflow in constant array size must be diagnosed. */
void f(void) {
  int a[2147483647 + 1];
  (void)a;
}
int main(void) { return 0; }
