/* C17 §6.5.6p3: subtracting _Atomic T* and T* violates compatible-type constraint */
long f(_Atomic int *p, int *q) { return p - q; }
int main(void) { return 0; }
