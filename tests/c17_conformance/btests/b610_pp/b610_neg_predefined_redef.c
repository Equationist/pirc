/* ISO C17 §6.10.8p2 — Predefined macro names cannot be #define'd. */
#define __LINE__ 42
int x = __LINE__;
