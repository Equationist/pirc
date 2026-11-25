/* C99 §6.7.2.1 p4: Bitfield cannot have floating-point type */
struct S {
    double bf : 4;
};
