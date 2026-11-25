/* C99 §6.7 p7: Defining object with incomplete struct type */
struct Incomplete;
int f(void) {
    struct Incomplete obj;
    (void)obj;
    return 0;
}
