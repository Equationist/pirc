/*
 * C17 Conformance Test — Class B (Negative, §6.7.2.2p2)
 * Constraint: "The expression that defines the value of an enumeration constant
 * shall be an integer constant expression that has a value representable as an int."
 */
enum BigEnum {
    TOO_BIG = 1ULL << 40
};

int main(void) {
    return (int)TOO_BIG;
}
