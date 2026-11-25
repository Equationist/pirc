/* C17 §6.7.2.4 p3: _Atomic type specifier shall not be applied to an array type */
_Atomic(int[4]) bad_atomic_arr;
