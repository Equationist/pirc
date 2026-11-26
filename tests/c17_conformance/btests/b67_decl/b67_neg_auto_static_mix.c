/* ISO C17 §6.7.1p2 — At most one storage-class specifier (auto + static invalid). */
int f(void) {
    auto static int x = 1;
    return x;
}
