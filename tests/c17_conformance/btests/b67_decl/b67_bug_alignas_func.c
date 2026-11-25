/*
 * C17 Conformance Test — Class B (Negative, §6.7.5p2)
 * Constraint: "An alignment specifier shall appear only in the declaration
 * specifiers of an object declaration ... shall not be specified in a
 * declaration of a typedef, or a bit-field, or a function, or a parameter..."
 */
_Alignas(16) void bad_aligned_func(void);

int main(void) {
    return 0;
}
