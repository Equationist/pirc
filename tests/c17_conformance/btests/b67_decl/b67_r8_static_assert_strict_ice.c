/* C17 §6.7.10p2: _Static_assert requires a strict integer constant expression */
static const int k = 5;
_Static_assert(k == 5, "non-ICE");
int main(void) { return 0; }
