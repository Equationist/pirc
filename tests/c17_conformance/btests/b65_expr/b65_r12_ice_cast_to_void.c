/* C17 6.6p6: cast to void is invalid inside an integer constant expression */
_Static_assert((0 && ((void)0, 1)) == 0, "");
