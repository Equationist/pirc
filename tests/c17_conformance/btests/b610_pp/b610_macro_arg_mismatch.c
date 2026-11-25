/* C99 §6.10.3 p4: Function-like macro invoked with wrong number of arguments */
#define ADD(a, b) ((a) + (b))
int x = ADD(1);
