/* C17 §6.7.7: _Alignas is not permitted in a type-name */
int x = (int)sizeof(_Alignas(8) int);
