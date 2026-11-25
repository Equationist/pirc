/*
 * C17 Conformance Test — Class B (Negative, §6.7.2.1p18)
 * Constraint: Static/aggregate initialization of a flexible array member
 * is a GNU extension, not permitted in ISO C17.
 */
struct F {
    int n;
    int a[];
};

struct F g_bad_fam = { 1, { 2, 3 } };

int main(void) {
    return g_bad_fam.n;
}
