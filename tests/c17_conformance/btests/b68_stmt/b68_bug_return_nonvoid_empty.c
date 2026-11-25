/*
 * C17 Conformance Test — Class B (Negative, §6.8.6.4p1)
 * Constraint: A return statement without an expression shall only appear in
 * a function whose return type is void.
 */
static int bad_empty_return(void) {
    return;
}

int main(void) {
    return bad_empty_return();
}
