/*
 * C17 Conformance Test — Class B (Negative, §6.5.16.1p1)
 * Constraint: Implicitly assigning/initializing an integer (other than _Bool)
 * from a pointer without a cast is a constraint violation.
 */
int main(void) {
    int x = 1;
    int addr = &x;
    return addr;
}
