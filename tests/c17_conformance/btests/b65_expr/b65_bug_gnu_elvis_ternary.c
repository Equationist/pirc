/*
 * C17 Conformance Test — Class B (Negative, §6.5.15)
 * Syntax: GNU conditional expression with omitted middle operand 'a ?: b'
 * is not valid ISO C17.
 */
int main(void) {
    int x = 1 ?: 2;
    return x;
}
