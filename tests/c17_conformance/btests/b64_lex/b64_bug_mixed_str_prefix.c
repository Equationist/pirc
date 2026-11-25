/*
 * C17 Conformance Test — Class B (Negative, §6.4.5p5)
 * Constraint/ISO C17: Concatenating string literals with incompatible
 * encoding prefixes (u8"" and u"") is invalid.
 */
const void *g_bad_str = u8"a" u"b";

int main(void) {
    return g_bad_str != 0;
}
