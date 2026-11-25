/*
 * C17 Conformance Test — Class B (Negative, §6.7.9p2)
 * Constraint: Excess elements in a numeric array initializer are forbidden.
 */
int main(void) {
    int a[2] = { 1, 2, 3 };
    return a[0];
}
