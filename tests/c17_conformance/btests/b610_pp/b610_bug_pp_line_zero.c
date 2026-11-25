/*
 * C17 Conformance Test — Class B (Negative, §6.10.4p3)
 * Constraint: The digit sequence in a #line directive shall specify a
 * line number strictly greater than zero (1..2147483647).
 */
#line 0

int main(void) {
    return 0;
}
