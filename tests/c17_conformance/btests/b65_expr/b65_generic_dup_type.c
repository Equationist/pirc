/* C17 §6.5.1.1 p2: Duplicate compatible types in _Generic association list */
int f(int x) {
    typedef int IntAlias;
    return _Generic(x, int: 1, IntAlias: 2);
}
