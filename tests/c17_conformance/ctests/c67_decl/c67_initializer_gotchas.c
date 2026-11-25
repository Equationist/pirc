/*
 * C17 §6.7.9: Initializer gotchas — braced scalar init, overriding a single
 * character inside a string-initialized struct member, deeply nested partial
 * zero-fill, union non-first member init, and global member/element relocs.
 */
#include "c17_test.h"

struct StrAndInt {
    char s[6];
    int k;
};

struct Deep {
    struct {
        int a[3];
    } b[2];
    int tail;
};

struct GlobalTarget {
    int pad;
    int arr[4];
    struct {
        int x;
        int y;
    } sub;
};

static struct GlobalTarget g_tgt = { 1, { 10, 20, 30, 40 }, { 50, 60 } };
static int *g_ptr_arr = &g_tgt.arr[2];
static int *g_ptr_sub = &g_tgt.sub.y;
static char g_str[] = "abcdef";
static char *g_str_mid = g_str + 3;

/* Global struct with string init + single char override */
static struct StrAndInt g_si = { .s = "hello", .s[1] = 'A', .k = 7 };

int main(void) {
    C17_BEGIN("c67_initializer_gotchas", "Braced scalars, string-member override, deep zero-fill, global relocs");

    /* Braced scalar initializer (§6.7.9 p11) */
    int sc = { 42 };
    double sd = { 3.5 };
    C17_CHECK_EQ(sc, 42);
    C17_CHECK_DBL(sd, 3.5, 0.0);

    /* Global string-member + element override */
    C17_CHECK_STR(g_si.s, "hAllo");
    C17_CHECK_EQ(g_si.k, 7);

    /* Local string-member + element override */
    struct StrAndInt l_si = { .s = "world", .s[0] = 'W', .s[4] = '!', .k = 9 };
    C17_CHECK_STR(l_si.s, "Worl!");
    C17_CHECK_EQ(l_si.k, 9);

    /* Deeply nested designated init zero-fills ALL unspecified elements */
    struct Deep d = { .b[1].a[2] = 99 };
    C17_CHECK_EQ(d.b[0].a[0], 0);
    C17_CHECK_EQ(d.b[0].a[1], 0);
    C17_CHECK_EQ(d.b[0].a[2], 0);
    C17_CHECK_EQ(d.b[1].a[0], 0);
    C17_CHECK_EQ(d.b[1].a[1], 0);
    C17_CHECK_EQ(d.b[1].a[2], 99);
    C17_CHECK_EQ(d.tail, 0);

    /* Global subobject pointer relocations */
    C17_CHECK_EQ(*g_ptr_arr, 30);
    C17_CHECK_EQ(*g_ptr_sub, 60);
    C17_CHECK_STR(g_str_mid, "def");

    C17_END();
}
