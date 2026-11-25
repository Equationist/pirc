/* C17 §6.7.5 p3: _Alignas alignment must be a power of two (or zero) */
_Alignas(3) int bad_align = 0;
