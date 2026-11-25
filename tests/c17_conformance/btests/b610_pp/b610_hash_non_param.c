/* C99 §6.10.3.2 p1: '#' in function-like macro must be followed by a parameter */
#define BAD(a) #not_a_param
const char *s = BAD(1);
