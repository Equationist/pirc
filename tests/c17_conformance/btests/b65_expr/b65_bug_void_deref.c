/*
 * C17 Conformance Test — Class B (Negative, §6.5.3.2p4)
 * Constraint/ISO C17: Dereferencing a pointer to void is not permitted.
 */
int main(void) {
    void *p = 0;
    (void)*p;
    return 0;
}
