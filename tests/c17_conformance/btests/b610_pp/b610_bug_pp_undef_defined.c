/*
 * C17 Conformance Test — Class B (Negative, §6.10.8p4)
 * Constraint: The identifier 'defined' shall not be the subject of #undef.
 */
#undef defined

int main(void) {
    return 0;
}
