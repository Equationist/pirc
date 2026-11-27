/* C17 §6.7.5p2: _Alignas is not allowed in compound literals */
int main(void) {
    (void)(_Alignas(16) int){0};
    return 0;
}
