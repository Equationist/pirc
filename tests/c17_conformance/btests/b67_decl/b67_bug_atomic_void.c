/*
 * C17 Conformance Test — Class B (Negative, §6.7.2.4p3 / §6.7.3p3)
 * Constraint: The type name in an atomic type specifier / _Atomic qualifier
 * shall not refer to an array type, a function type, or an incomplete type
 * such as 'void'.
 */
_Atomic void bad_atomic_void_fn(void);

int main(void) {
    return 0;
}
