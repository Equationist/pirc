/*
 * C17 Conformance Test — Class B (Negative, §6.7.2.1)
 * Syntax: Stray extra semicolon inside a struct definition is not valid ISO C17.
 */
struct BadSemi {
    int a;
    ;
};

int main(void) {
    return 0;
}
