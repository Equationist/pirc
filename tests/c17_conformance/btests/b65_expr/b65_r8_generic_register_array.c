/* C17 §6.3.2.1p3: register array cannot decay to pointer in _Generic */
int main(void) {
    register int a[5];
    return _Generic(a, int *: 1, default: 2);
}
