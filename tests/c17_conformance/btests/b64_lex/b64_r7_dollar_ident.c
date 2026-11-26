/* C17 §6.4.2.1: '$' is not a valid identifier character in strict ISO C17. */
int main(void) {
    int a$b = 0;
    return a$b;
}
