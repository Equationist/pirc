/* C99 §6.7.2.1 p3: Named bitfield cannot have width zero */
struct S {
    int bad : 0;
};
