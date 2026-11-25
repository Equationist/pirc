/*
 * C17 Conformance Test — Class B (Negative, §6.5.3.4p1)
 * Syntax/Constraint: _Alignof may only be applied to a parenthesized type-name,
 * not an expression (applying _Alignof to an expression is a GNU extension).
 */
int main(void) {
    int x = 1;
    return (int)_Alignof(x);
}
