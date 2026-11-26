/* ISO C17 §6.7.6.3p15 — K&R 0-param definition conflicts with 1-param prototype. */
int f(int x);
int f() { return 0; }
