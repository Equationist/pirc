/* C17 §6.7.3p2: _Atomic cannot qualify an incomplete type */
struct S;
_Atomic struct S *p;
int main(void) { return p != 0; }
