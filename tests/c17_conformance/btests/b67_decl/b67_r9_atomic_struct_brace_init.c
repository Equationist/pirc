/* C17 §6.7.9: _Atomic struct cannot be initialized with brace-enclosed member list */
struct S { int x; };
int main(void) {
    _Atomic struct S s = { 1 };
    (void)s;
    return 0;
}
