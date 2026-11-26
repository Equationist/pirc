/* ISO C17 §6.10.3 — GNU named variadic macro parameter 'args...' is not allowed in ISO C17. */
#define M(args...) (args)
int x = 0;
