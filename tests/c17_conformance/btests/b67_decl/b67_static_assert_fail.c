/* C17 §6.7.10 p2: Failing _Static_assert must produce a diagnostic */
_Static_assert(0, "intentional static assertion failure");
int x = 0;
