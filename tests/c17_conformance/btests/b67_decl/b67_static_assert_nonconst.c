/* C17 §6.7.10 p2: _Static_assert condition must be an integer constant expression */
void f(int x) {
    _Static_assert(x == 0, "not a constant expression");
}
