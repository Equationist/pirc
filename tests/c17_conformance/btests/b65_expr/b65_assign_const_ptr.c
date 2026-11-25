/* C99 §6.5.16.1: Modification through pointer to const */
void f(const int *p) {
    *p = 10;
}
