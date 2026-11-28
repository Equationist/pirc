/* C17 6.7.4p3: extern inline function cannot reference identifier with internal linkage. */
static int s = 1;
extern inline int f(void) { return s; }
int main(void) { return f(); }
