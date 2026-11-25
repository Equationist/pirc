/*
 * C17 Conformance Test — Class B (Negative, §6.7.9)
 * Syntax: GNU array designator range '[lo ... hi]' is not valid ISO C17.
 */
int main(void) {
    int a[4] = { [0 ... 3] = 1 };
    return a[0];
}
