/*
 * C17 §6.7 p3: Redeclaring a typedef to refer to the same type in the same
 * scope is valid in ISO C11/C17 (unlike C99).
 */

typedef int MyInt;
typedef int MyInt;

typedef int Arr4[4];
typedef int Arr4[4];

struct S { int x; };
typedef struct S SType;
typedef struct S SType;

int use_redecl_typedefs(void) {
    typedef double Dbl;
    typedef double Dbl;
    MyInt a = 1;
    Arr4 arr = { 1, 2, 3, 4 };
    SType s = { 5 };
    Dbl d = 2.0;
    return a + arr[3] + s.x + (int)d;
}
