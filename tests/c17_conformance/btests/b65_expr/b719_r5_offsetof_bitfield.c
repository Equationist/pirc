/* C17 §7.19p3: offsetof shall not be applied to a bit-field member */
#include <stddef.h>
struct S { int a : 3; int b : 5; };
size_t f(void) {
    return offsetof(struct S, b);
}
