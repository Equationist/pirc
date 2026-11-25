/*
 * C17 Conformance Test — Class B (Negative, §6.9p2)
 * Constraint: The storage-class specifiers 'auto' and 'register' shall not
 * appear in the declaration specifiers in an external declaration.
 */
register int g_bad_register = 1;

int main(void) {
    return g_bad_register;
}
