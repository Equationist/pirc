/*
 * C17 Conformance Test — Class B (Negative, §6.7.9p14 / §6.7.9p2)
 * Constraint: String literal initializer with more non-NUL characters than
 * elements in the target char array is a constraint violation.
 */
int main(void) {
    char s[2] = "abc";
    return s[0];
}
