/* ISO C17 §6.7.3p2: _Atomic qualifier shall not be applied to an array type */
typedef int Arr4[4];
_Atomic Arr4 a;
