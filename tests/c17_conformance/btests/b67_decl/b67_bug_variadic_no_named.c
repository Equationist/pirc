/*
 * C17 Conformance Test — Class B (Negative, §6.7.6.3)
 * Syntax: In ISO C17, a variadic function parameter list requires at least
 * one named parameter before '...' ('void f(...)' was added in C23).
 */
int bad_variadic(...);

int main(void) {
    return 0;
}
