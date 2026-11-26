/* C17 §6.5.2.2, §6.6, §6.10.3.2:
 * - Static _Bool initialization from address equality/inequality & logical not
 * - Cast function pointer calls with signext/zeroext and variadic mismatch
 * - Preprocessor stringize escaping of control characters */
#include "c17_test.h"
#include <stdarg.h>
#include <string.h>

static int g_x = 1, g_y = 2;
static _Bool g_eq = (&g_x == &g_y);
static _Bool g_ne = (&g_x != &g_y);
static _Bool g_not = !&g_x;

static int real_schar(signed char c) {
    return (int)c;
}

static signed char real_ret_schar(void) {
    return (signed char)-5;
}

static int real_va_sum(int a, ...) {
    va_list ap;
    va_start(ap, a);
    int b = va_arg(ap, int);
    va_end(ap);
    return a * 100 + b;
}

#define STR(x) #x

int main(void) {
    C17_CHECK(!g_eq);
    C17_CHECK(g_ne);
    C17_CHECK(!g_not);

    /* Cast through incompatible pointer type and back to true type (C17 §6.3.2.3p8) */
    void (*erased1)(void) = (void (*)(void))real_schar;
    void (*erased2)(void) = (void (*)(void))real_ret_schar;
    void (*erased3)(void) = (void (*)(void))real_va_sum;

    int r1 = ((int (*)(signed char))erased1)((signed char)-5);
    signed char r2 = ((signed char (*)(void))erased2)();
    int r3 = ((int (*)(int, ...))erased3)(10, 20);
    C17_CHECK_EQ(r1, -5);
    C17_CHECK_EQ((int)r2, -5);
    C17_CHECK_EQ(r3, 1020);

    /* Stringize preserves backslash escapes cleanly */
    const char *s = STR("a\tb");
    C17_CHECK(strcmp(s, "\"a\\tb\"") == 0);

    C17_END();
}
