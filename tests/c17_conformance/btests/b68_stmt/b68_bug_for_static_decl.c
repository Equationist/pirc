/*
 * C17 Conformance Test — Class B (Negative, §6.8.5p3)
 * Constraint: The declaration part of a for statement shall only declare
 * identifiers for objects having storage class auto or register.
 */
int main(void) {
    for (static int i = 0; i < 3; i++) {}
    return 0;
}
