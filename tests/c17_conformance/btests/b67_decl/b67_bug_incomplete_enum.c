/*
 * C17 Conformance Test — Class B (Negative, §6.7.2.3p2)
 * Constraint: A type specifier of the form 'enum identifier' without an
 * enumerator list shall only appear after the type it specifies is complete.
 */
enum ForwardEnum *p;

int main(void) {
    return p != 0;
}
