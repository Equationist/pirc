/* C99 §6.7.2.1 p3: Bitfield width cannot be negative */
struct S {
    int neg : -1;
};
