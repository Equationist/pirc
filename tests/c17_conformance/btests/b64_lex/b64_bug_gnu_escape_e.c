/*
 * C17 Conformance Test — Class B (Negative, §6.4.4.4)
 * Constraint: '\e' escape sequence is a GNU extension, not valid ISO C17.
 */
int main(void) {
    int c = '\e';
    return c;
}
