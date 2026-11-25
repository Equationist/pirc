/*
 * C17 Conformance Test — Class B (Negative, §6.7.2.1p5)
 * Constraint: A bit-field cannot have an _Atomic-qualified type.
 */
struct BadAtomicBf {
    _Atomic int bits : 3;
};

int main(void) {
    return 0;
}
