/*
 * C17 Conformance Test — Class B (Negative, §6.7.4p2)
 * Constraint: Function specifiers ('inline', '_Noreturn') shall be used only
 * in the declaration of an identifier for a function.
 */
_Noreturn int g_bad_noreturn = 1;

int main(void) {
    return g_bad_noreturn;
}
