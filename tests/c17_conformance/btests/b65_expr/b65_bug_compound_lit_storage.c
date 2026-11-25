/*
 * C17 Conformance Test — Class B (Negative, §6.5.2.5p1)
 * Constraint: Storage-class specifiers in compound literals are a C23 feature
 * and are not permitted in ISO C17.
 */
int main(void) {
    int *p = &(static int){ 42 };
    return *p;
}
