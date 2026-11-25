/*
 * C17 Conformance Test — Class B (Negative, §6.7.10)
 * Constraint: In ISO C17, _Static_assert requires a string literal message
 * operand (1-argument _Static_assert was added in C23).
 */
_Static_assert(1);

int main(void) {
    return 0;
}
