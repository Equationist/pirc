/* ISO C17 §6.10.3.3 — GNU ', ## __VA_ARGS__' comma-elision extension is not allowed in ISO C17. */
#define M(a, ...) f(a, ##__VA_ARGS__)
int f(int a) { return a; }
int g(void) { return M(1); }
