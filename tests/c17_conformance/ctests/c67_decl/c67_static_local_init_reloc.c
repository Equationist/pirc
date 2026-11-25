/*
 * C99 §6.2.4 & §6.7.8: Multiple functions with same-named static local
 * variables, static local self-referential pointers, and relocations.
 */
#include "c17_test.h"

struct Node {
    int val;
    const struct Node *next;
};

static int fn_alpha(void) {
    static int counter = 10;
    counter += 1;
    return counter;
}

static int fn_beta(void) {
    static int counter = 100;
    counter += 5;
    return counter;
}

static int fn_nested_blocks(int which) {
    int r = 0;
    if (which == 0) {
        static int counter = 1000;
        counter += 1;
        r = counter;
    } else {
        static int counter = 2000;
        counter += 2;
        r = counter;
    }
    return r;
}

static int fn_static_list(void) {
    static const struct Node n3 = { 30, NULL };
    static const struct Node n2 = { 20, &n3 };
    static const struct Node n1 = { 10, &n2 };
    int sum = 0;
    for (const struct Node *p = &n1; p != NULL; p = p->next) {
        sum += p->val;
    }
    return sum;
}

int main(void) {
    C99_BEGIN("c67_static_local_init_reloc", "Same-named static locals & static local pointer relocs");

    C99_CHECK_EQ(fn_alpha(), 11);
    C99_CHECK_EQ(fn_beta(), 105);
    C99_CHECK_EQ(fn_alpha(), 12);
    C99_CHECK_EQ(fn_beta(), 110);

    /* Same-named static locals in sibling blocks of the SAME function! */
    C99_CHECK_EQ(fn_nested_blocks(0), 1001);
    C99_CHECK_EQ(fn_nested_blocks(1), 2002);
    C99_CHECK_EQ(fn_nested_blocks(0), 1002);
    C99_CHECK_EQ(fn_nested_blocks(1), 2004);

    /* Static local linked list with cross-static relocations */
    C99_CHECK_EQ(fn_static_list(), 60);

    C99_END();
}
