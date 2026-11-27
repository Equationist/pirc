/* C17 §6.10.8p2: _Pragma cannot be redefined as a macro */
#define _Pragma 1
int main(void) { return _Pragma; }
