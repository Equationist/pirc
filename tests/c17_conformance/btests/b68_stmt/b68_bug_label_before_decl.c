/*
 * C17 Conformance Test — Class B (Negative, §6.8.1)
 * Syntax: In ISO C17, a label may only precede a statement, not a declaration
 * (labels before declarations were added in C23).
 */
int main(void) {
L:
    int y = 2;
    return y;
}
