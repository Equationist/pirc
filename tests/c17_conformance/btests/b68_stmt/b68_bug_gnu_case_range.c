/*
 * C17 Conformance Test — Class B (Negative, §6.8.1)
 * Syntax: GNU switch case range 'case lo ... hi:' is not valid ISO C17.
 */
static int bad_range_sw(int x) {
    switch (x) {
        case 1 ... 3: return 1;
        default:      return 0;
    }
}

int main(void) {
    return bad_range_sw(2);
}
