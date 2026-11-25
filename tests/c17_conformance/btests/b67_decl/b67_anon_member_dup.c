/* C17 §6.7.2.1: Duplicate member name introduced by anonymous struct */
struct S {
    int x;
    struct {
        int x;
        int y;
    };
};
