/* ISO C17 §6.10.1 — C23 #elifdef directive is not allowed in ISO C17. */
#ifdef A
int x = 1;
#elifdef B
int x = 2;
#else
int x = 3;
#endif
