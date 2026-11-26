/* ISO C17 §6.7.2.1p18 — Flexible array member in struct whose only preceding member is an anonymous struct with unnamed bitfield. */
struct S {
    struct {
        int : 4;
    };
    int a[];
};
