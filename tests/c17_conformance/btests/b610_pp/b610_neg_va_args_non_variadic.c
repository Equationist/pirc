/* ISO C17 §6.10.3p5 — __VA_ARGS__ shall only appear in variadic macro replacement list. */
#define M(x) __VA_ARGS__
int x = 0;
