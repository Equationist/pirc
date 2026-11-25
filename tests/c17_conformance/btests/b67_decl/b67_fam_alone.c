/* C99 §6.7.2.1 p16: Flexible array member requires at least one preceding named member */
struct BadFamAlone {
    int data[];
};
