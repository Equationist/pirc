/*
 * C17 Conformance Test — Class B (Negative, §6.7.9p2)
 * Constraint: No initializer shall attempt to provide a value for an object
 * not contained within the entity being initialized (excess struct initializer).
 */
struct One {
    int a;
};

int main(void) {
    struct One s = { 1, 2 };
    return s.a;
}
