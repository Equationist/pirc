/*
 * C17 Conformance Test — Class B (Negative, §6.5.15p3)
 * Constraint: In a conditional expression, if one of the second or third
 * operands has type void, both must have type void.
 */
static void do_nothing(void) {}

int main(void) {
    1 ? do_nothing() : 0;
    return 0;
}
