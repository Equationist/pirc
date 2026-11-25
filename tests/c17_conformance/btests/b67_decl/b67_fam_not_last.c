/* C99 §6.7.2.1 p16: Flexible array member must be the last member of a struct */
struct BadFamNotLast {
    int n;
    char data[];
    int after;
};
