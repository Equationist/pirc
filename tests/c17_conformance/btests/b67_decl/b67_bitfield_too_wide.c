/* C99 §6.7.2.1 p3: Bitfield width cannot exceed width of its type */
struct S {
    unsigned int bf : 65;
};
