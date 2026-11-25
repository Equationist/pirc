/* C99 §6.5.16.1: Struct containing a const member is not a modifiable lvalue */
struct HasConst {
    const int id;
    int val;
};

void f(void) {
    struct HasConst a = { 1, 10 };
    struct HasConst b = { 2, 20 };
    a = b;
}
