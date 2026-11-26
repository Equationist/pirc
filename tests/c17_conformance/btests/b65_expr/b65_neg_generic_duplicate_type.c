/* ISO C17 §6.5.1.1p2: _Generic shall not contain duplicate compatible type names */
int x = _Generic(1, int: 1, int: 2);
