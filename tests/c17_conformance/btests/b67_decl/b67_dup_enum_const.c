/* C17 §6.7 p3: Duplicate enumerator name in same scope */
enum E {
    DUP_E = 1,
    DUP_E = 2
};
