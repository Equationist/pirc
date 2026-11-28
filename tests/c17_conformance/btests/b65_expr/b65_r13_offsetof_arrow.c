#include <stddef.h>
struct Inner { int y; };
struct Outer { struct Inner *p; };
int f(void) { return (int)offsetof(struct Outer, p->y); }
