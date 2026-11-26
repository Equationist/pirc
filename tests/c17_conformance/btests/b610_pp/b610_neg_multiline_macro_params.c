/* ISO C17 §6.10.3 — Function-like macro parameter list cannot span unescaped newlines. */
#define F(
x) (x)
int y = 0;
