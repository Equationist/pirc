/*
 * C17 Conformance Test — Class B (Negative, §6.5.1)
 * Syntax: GNU statement expression '({ ... })' is not valid ISO C17.
 */
int main(void) {
    int x = ({ int y = 2; y + 1; });
    return x;
}
