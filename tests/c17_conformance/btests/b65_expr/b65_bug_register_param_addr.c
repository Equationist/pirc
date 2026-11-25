/*
 * C17 Conformance Test — Class B (Negative, §6.7.1p6 / §6.5.3.2p1)
 * Constraint: Taking the address of a parameter declared with the 'register'
 * storage-class specifier is a constraint violation.
 */
static int bad_reg_param(register int x) {
    int *p = &x;
    return *p;
}

int main(void) {
    return bad_reg_param(1);
}
