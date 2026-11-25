/*
 * C17 Conformance Test — Class B (Negative, §6.7.9)
 * Syntax: GNU array designator omitting '=' ('[0] 1') is not valid ISO C17.
 */
int main(void) {
    int a[2] = { [0] 1 };
    return a[0];
}
