/* C99 §6.5.3.2 p1: Cannot take address of bitfield */
struct S { int bf : 3; };
int *f(struct S *s) {
    return &s->bf;
}
