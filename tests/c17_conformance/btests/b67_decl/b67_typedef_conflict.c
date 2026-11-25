/* C17 §6.7 p3: Conflicting typedef redeclaration in the same scope is forbidden */
typedef int MyType;
typedef double MyType;
int x = 0;
