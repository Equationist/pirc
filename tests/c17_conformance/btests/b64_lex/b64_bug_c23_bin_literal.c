/*
 * C17 Conformance Test — Class B (Negative, §6.4.4.1)
 * Syntax: Binary integer constant '0b1010' is a C23/GNU extension, not ISO C17.
 */
int main(void) {
    int x = 0b1010;
    return x;
}
