/* C17 §6.5.2.2: void expression passed as variadic argument */
int va_fn(int a, ...);
void f(void) {
    va_fn(1, (void)0);
}
