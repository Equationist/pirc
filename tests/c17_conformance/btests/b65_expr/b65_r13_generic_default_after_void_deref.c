int f(void *p) { return _Generic(1, int: 42, default: *p); }
