/*
 * C17 Conformance Test — Class B (Negative, §6.8.5p3)
 * Constraint: typedef is not permitted in a for-loop header declaration.
 */
int main(void) {
    for (typedef int T; 0; ) {}
    return 0;
}
