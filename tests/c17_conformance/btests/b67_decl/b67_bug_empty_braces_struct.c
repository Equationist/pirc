/*
 * C17 Conformance Test — Class B (Negative, §6.7.9p1)
 * Syntax: Empty initializer braces '{}' are a C23/GNU extension, invalid in ISO C17.
 */
struct S {
    int a;
};

int main(void) {
    struct S s = {};
    return s.a;
}
