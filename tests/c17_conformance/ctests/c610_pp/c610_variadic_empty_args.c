/*
 * C99 §6.10.3: Variadic macros (__VA_ARGS__), empty macro arguments,
 * ## placemarker tokens, # stringification escaping/whitespace collapsing,
 * prescan vs non-prescan, and macro recursion blue-paint rule.
 */
#include "c17_test.h"

#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a ## b
#define XCAT(a, b) CAT(a, b)

/* Variadic macro with named param + __VA_ARGS__ */
static int sum3(int a, int b, int c) { return a + b + c; }
#define CALL_SUM(fn, ...) fn(__VA_ARGS__)
#define FIRST_AND_REST(a, ...) ((a) + sum3(__VA_ARGS__))

/* Variadic macro stringizing __VA_ARGS__ */
#define VSTR(...) #__VA_ARGS__

/* Empty macro arguments & placemarker pasting (C99 §6.10.3 p4, §6.10.3.3 p2) */
#define PASTE3(a, b, c) a ## b ## c
#define SECOND(a, b) b

/* Self-referential and mutually recursive macros (blue-paint rule §6.10.3.4) */
static int self_fn(int x) { return x + 10; }
#define self_fn(x) (self_fn(x) + 1)

static int mut_a(int x) { return x * 2; }
#define mut_b(x) (mut_a(x) + 5)
#define mut_a(x) (mut_b(x) + 3)

/* Indirect prescan test */
#define VALUE 42

int main(void) {
    C99_BEGIN("c610_variadic_empty_args", "Variadic macros, empty args, placemarkers, #, ##, rescan");

    /* Variadic macros */
    C99_CHECK_EQ(CALL_SUM(sum3, 10, 20, 30), 60);
    C99_CHECK_EQ(FIRST_AND_REST(5, 1, 2, 3), 11);
    C99_CHECK_STR(VSTR(a,   b,  c), "a, b, c");
    C99_CHECK_STR(VSTR(), "");

    /* Empty arguments + placemarkers with ## */
    int foo = 123;
    int bar = 456;
    C99_CHECK_EQ(PASTE3(fo, , o), 123);
    C99_CHECK_EQ(PASTE3(, b, ar), 456);
    C99_CHECK_EQ(PASTE3(foo, , ), 123);
    /* Pasting two empty placemarkers yields empty token sequence */
    int z = 7 PASTE3(, , ) + 8;
    C99_CHECK_EQ(z, 15);
    C99_CHECK_EQ(SECOND(, 99), 99);
    C99_CHECK_STR(STR(), "");

    /* Stringification whitespace collapsing and escaping of " and \ */
    C99_CHECK_STR(STR(   hello    world   ), "hello world");
    C99_CHECK_STR(STR("a\nb"), "\"a\\nb\"");
    C99_CHECK_STR(STR(VALUE), "VALUE");
    C99_CHECK_STR(XSTR(VALUE), "42");

    /* Token paste does NOT prescan operands, indirect XCAT DOES */
    int var_42 = 88;
    int var_VALUE = 77;
    C99_CHECK_EQ(CAT(var_, VALUE), 77);
    C99_CHECK_EQ(XCAT(var_, VALUE), 88);

    /* Blue-paint recursion prevention */
    C99_CHECK_EQ(self_fn(5), 16);  /* self_fn(5) -> (self_fn(5) + 1) -> (15 + 1) */
    C99_CHECK_EQ(mut_a(10), 28);   /* ((mut_a(10) + 5) + 3) -> ((20 + 5) + 3) */

    C99_END();
}
