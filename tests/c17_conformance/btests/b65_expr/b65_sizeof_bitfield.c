/* C99 §6.5.3.4 p1: sizeof cannot be applied to a bitfield */
struct S { unsigned int bf : 5; };
unsigned long f(struct S *s) {
    return sizeof(s->bf);
}
