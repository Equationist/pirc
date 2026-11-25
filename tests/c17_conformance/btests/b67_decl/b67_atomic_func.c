/* C17 §6.7.2.4 p3: _Atomic type specifier shall not be applied to a function type */
_Atomic(int(void)) bad_atomic_fn;
