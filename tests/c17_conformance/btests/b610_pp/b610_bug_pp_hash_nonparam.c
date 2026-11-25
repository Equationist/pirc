/*
 * C17 Conformance Test — Class B (Negative, §6.10.3.2p1)
 * Constraint: Each '#' preprocessing token in the replacement list for a
 * function-like macro shall be followed by a parameter as the next token.
 */
#define BAD_STR(a) #not_a_param

int main(void) {
    return 0;
}
