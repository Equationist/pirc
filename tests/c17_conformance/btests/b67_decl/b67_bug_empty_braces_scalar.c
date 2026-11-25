/*
 * C17 Conformance Test — Class B (Negative, §6.7.9p1)
 * Syntax: Empty scalar initializer '{}' is a C23/GNU extension, invalid in ISO C17.
 */
int main(void) {
    int x = {};
    return x;
}
