/*
 * C17 §6.2.1 & §6.7.2: Defining struct/enum types inside sizeof() and compound
 * literal casts, enum constants declared inside struct leaking to enclosing
 * scope (C vs C++ difference!), and block-scoped typedef re-shadowing.
 */
#include "c17_test.h"

typedef int OuterT;

struct WithInnerEnum {
    enum InnerE {
        IN_A = 11,
        IN_B = 22
    } tag;
    int val;
};

/* Forward tag used in typedef before struct definition */
struct FwdNode;
typedef struct FwdNode FwdNode;
struct FwdNode {
    int id;
    const FwdNode *next;
};

int main(void) {
    C17_BEGIN("c67_struct_tag_and_typedef_gotchas", "Type decls in sizeof/casts, struct-inner enum scope, typedef shadowing");

    /* Enumerators declared inside a struct body are visible in enclosing scope in C! */
    C17_CHECK_EQ(IN_A, 11);
    C17_CHECK_EQ(IN_B, 22);
    struct WithInnerEnum w = { IN_B, 7 };
    C17_CHECK_EQ(w.tag, 22);

    /* Struct type defined inside sizeof(...) */
    size_t sz = sizeof(struct InlineSz { int a; double b; });
    C17_CHECK(sz >= sizeof(int) + sizeof(double));
    /* Struct InlineSz is now in scope for the rest of this block! */
    struct InlineSz ins = { 3, 4.5 };
    C17_CHECK_EQ(ins.a, 3);
    C17_CHECK_DBL(ins.b, 4.5, 0.0);

    /* Struct type defined inside a compound literal cast */
    int v = ((struct InlineCL { int x; int y; }){ .y = 30, .x = 12 }).x + 1;
    C17_CHECK_EQ(v, 13);

    /* Block-scope typedef shadowing outer typedef with a DIFFERENT type */
    OuterT a = 5;
    double d_seen = 0.0;
    {
        typedef double OuterT;
        OuterT d = 3.25;
        d_seen = d;
        C17_CHECK_EQ(sizeof(OuterT), sizeof(double));
    }
    C17_CHECK_EQ(sizeof(OuterT), sizeof(int));
    C17_CHECK_EQ(a, 5);
    C17_CHECK_DBL(d_seen, 3.25, 0.0);

    /* Forward typedef recursive struct */
    FwdNode n2 = { 2, NULL };
    FwdNode n1 = { 1, &n2 };
    C17_CHECK_EQ(n1.id + n1.next->id, 3);

    C17_END();
}
