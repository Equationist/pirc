/*
 * C17 Conformance Test — Class B (Negative, §6.8.5p3)
 * Constraint: extern declaration is not permitted in a for-loop header.
 */
int main(void) {
    for (extern int i; 0; ) {}
    return 0;
}
