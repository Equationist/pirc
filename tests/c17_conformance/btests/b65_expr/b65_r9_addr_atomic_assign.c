/* C17 §6.5.16p3: Assignment expression to _Atomic object is not an lvalue */
void f(void) {
    _Atomic int x = 0;
    int *p = &(x = 1);
    (void)p;
}
int main(void) { return 0; }
