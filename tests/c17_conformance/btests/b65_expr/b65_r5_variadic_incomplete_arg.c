/* C17 §6.5.2.2: incomplete struct type passed as variadic argument */
struct Incomplete;
extern struct Incomplete inc;
int va_fn(int a, ...);
void f(void) {
    va_fn(1, inc);
}
