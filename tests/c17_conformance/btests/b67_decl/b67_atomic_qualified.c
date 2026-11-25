/* C17 §6.7.2.4 p3: _Atomic(type-name) shall not specify a qualified type */
_Atomic(const int) bad_atomic_const;
