/*
 * C17 Conformance Test — Class B (Negative, §6.10.1p1)
 * Syntax: Extra preprocessing tokens after #endif are not permitted in ISO C17.
 */
#if 1
int ok_var = 1;
#endif trailing_junk

int main(void) {
    return ok_var;
}
