/*
 * C17 Conformance Test — Class B (Negative, §6.7.2.1p18)
 * Constraint: A flexible array member shall only appear as the last member of
 * a structure with more than one named member (unnamed bit-fields do not count).
 */
struct BadFamUnnamedBf {
    int : 0;
    int flex[];
};

int main(void) {
    return 0;
}
